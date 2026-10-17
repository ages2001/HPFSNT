/*
 * HPFSNT - IRP_MJ_READ
 */

#include "hpfs.h"

/* DASD reads of the whole volume */
static NTSTATUS
HpfsReadVolume (
    PHPFS_IRP_CONTEXT Ctx,
    PHPFS_FCB Vfcb,
    PHPFS_CCB Ccb,
    LONGLONG StartingByte,
    ULONG ByteCount
    )
{
    PHPFS_VCB Vcb = Vfcb->Vcb;
    PIRP Irp = Ctx->Irp;
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    LONGLONG Limit;
    NTSTATUS Status;

    if (Ccb == NULL || (Irp->Flags & IRP_PAGING_IO)) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (!(Ccb->Flags & CCB_FLAG_DISMOUNTED_VOLUME) && Vcb->LockFileObject != FileObject) {
        Status = HpfsVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }

    if (((ULONG)StartingByte & (HPFS_SECTOR_SIZE - 1)) != 0 ||
        (ByteCount & (HPFS_SECTOR_SIZE - 1)) != 0) {
        return STATUS_INVALID_PARAMETER;
    }

    Limit = Vcb->PartitionBytes;
    if (StartingByte >= Limit) {
        return STATUS_END_OF_FILE;
    }
    if (StartingByte + ByteCount > Limit) {
        ByteCount = (ULONG)(Limit - StartingByte);
    }

    /* What the engine has changed must be on the disk before it is read raw */
    HpfsEngineEnter(Vcb);
    (VOID)HpfsEngineLeave(Vcb, STATUS_SUCCESS);

    Status = HpfsLockUserBuffer(Irp, IoWriteAccess, ByteCount);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Status = HpfsNonCachedIo(Ctx, Vfcb, IRP_MJ_READ, StartingByte, ByteCount, Limit);

    if (NT_SUCCESS(Status)) {
        Irp->IoStatus.Information = ByteCount;
        if (FileObject->Flags & FO_SYNCHRONOUS_IO) {
            FileObject->CurrentByteOffset.QuadPart = StartingByte + ByteCount;
        }
    }

    return Status;
}

/* The cached read, run through HpfsTryCall (the Cache Manager raises) */
typedef struct _HPFS_CC_READ {
    PFILE_OBJECT FileObject;
    PHPFS_FCB   Fcb;
    PIRP        Irp;
    BOOLEAN     Mdl;
    LARGE_INTEGER Start;
    ULONG       Count;
    PVOID       Buffer;
    NTSTATUS    Status;
} HPFS_CC_READ;

static VOID
HpfsCcReadBody (
    PVOID Context
    )
{
    HPFS_CC_READ *C = (HPFS_CC_READ *)Context;

    if (C->FileObject->PrivateCacheMap == NULL) {
        CcInitializeCacheMap(C->FileObject, (PCC_FILE_SIZES)&C->Fcb->Header.AllocationSize, FALSE,
                             &HpfsData.CacheManagerCallbacks, C->Fcb);
    }

    if (C->Mdl) {
        CcMdlRead(C->FileObject, &C->Start, C->Count, &C->Irp->MdlAddress, &C->Irp->IoStatus);
        C->Status = C->Irp->IoStatus.Status;
    } else if (!CcCopyRead(C->FileObject, &C->Start, C->Count, TRUE, C->Buffer, &C->Irp->IoStatus)) {
        C->Status = STATUS_UNSUCCESSFUL;
    } else {
        C->Status = C->Irp->IoStatus.Status;
    }
}

static NTSTATUS
HpfsReadFile (
    PHPFS_IRP_CONTEXT Ctx,
    PHPFS_FCB Fcb,
    LARGE_INTEGER StartingByte,
    ULONG ByteCount
    )
{
    PHPFS_VCB Vcb = Fcb->Vcb;
    PIRP Irp = Ctx->Irp;
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    BOOLEAN PagingIo = (BOOLEAN)((Irp->Flags & IRP_PAGING_IO) != 0);
    BOOLEAN NonCached = (BOOLEAN)((Irp->Flags & IRP_NOCACHE) != 0);
    BOOLEAN Acquired = FALSE;
    BOOLEAN PagingAcquired = FALSE;
    IO_STATUS_BLOCK Iosb;
    LONGLONG FileSize;
    ULONG Transfer;
    PVOID Buffer;
    HPFS_CC_READ Cc;
    NTSTATUS Code;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!PagingIo) {

        Status = HpfsVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }

        if (NonCached &&
            (((ULONG)StartingByte.QuadPart & (HPFS_SECTOR_SIZE - 1)) != 0 ||
             (ByteCount & (HPFS_SECTOR_SIZE - 1)) != 0)) {
            return STATUS_INVALID_PARAMETER;
        }
    }

    __try {

        if (PagingIo) {
            (VOID)ExAcquireResourceSharedLite(&Fcb->PagingIoResource, TRUE);
            PagingAcquired = TRUE;
        } else {
            (VOID)ExAcquireResourceSharedLite(&Fcb->Resource, TRUE);
            Acquired = TRUE;

            if (!FsRtlCheckLockForReadAccess(&Fcb->FileLock, Irp)) {
                Status = STATUS_FILE_LOCK_CONFLICT;
                __leave;
            }

            if (NonCached && Fcb->SectionObjectPointers.DataSectionObject != NULL) {
                CcFlushCache(&Fcb->SectionObjectPointers, &StartingByte, ByteCount, &Iosb);
                if (!NT_SUCCESS(Iosb.Status)) {
                    Status = Iosb.Status;
                    __leave;
                }
            }
        }

        FileSize = Fcb->Header.FileSize.QuadPart;

        if (StartingByte.QuadPart >= FileSize) {
            Irp->IoStatus.Information = 0;
            Status = STATUS_END_OF_FILE;
            __leave;
        }

        if (StartingByte.QuadPart + ByteCount > FileSize) {
            ByteCount = (ULONG)(FileSize - StartingByte.QuadPart);
        }

        if (!NonCached) {

            Buffer = NULL;
            if (!(IrpSp->MinorFunction & IRP_MN_MDL)) {
                Buffer = HpfsMapUserBuffer(Irp);
                if (Buffer == NULL) {
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    __leave;
                }
            }

            Cc.FileObject = FileObject;
            Cc.Fcb = Fcb;
            Cc.Irp = Irp;
            Cc.Mdl = (BOOLEAN)((IrpSp->MinorFunction & IRP_MN_MDL) != 0);
            Cc.Start = StartingByte;
            Cc.Count = ByteCount;
            Cc.Buffer = Buffer;
            Code = HpfsTryCall(HpfsCcReadBody, &Cc, FALSE);
            Status = NT_SUCCESS(Code) ? Cc.Status : HpfsExceptionStatus(Code);

            if (!NT_SUCCESS(Status)) {
                __leave;
            }

        } else {

            Transfer = (ULONG)HpfsRoundUp(ByteCount, HPFS_SECTOR_SIZE);

            if (!PagingIo) {
                Status = HpfsLockUserBuffer(Irp, IoWriteAccess, Transfer);
                if (!NT_SUCCESS(Status)) {
                    __leave;
                }
            }

            Status = HpfsNonCachedIo(Ctx, Fcb, IRP_MJ_READ, StartingByte.QuadPart, Transfer,
                                     Fcb->Header.ValidDataLength.QuadPart);
        }

        if (NT_SUCCESS(Status)) {
            Irp->IoStatus.Information = ByteCount;
            if (!PagingIo && (FileObject->Flags & FO_SYNCHRONOUS_IO)) {
                FileObject->CurrentByteOffset.QuadPart = StartingByte.QuadPart + ByteCount;
            }
        }

    } __finally {

        if (Acquired) {
            HpfsRelease(&Fcb->Resource);
        }
        if (PagingAcquired) {
            HpfsRelease(&Fcb->PagingIoResource);
        }
    }

    return Status;
}

NTSTATUS
HpfsCommonRead (
    PHPFS_IRP_CONTEXT Ctx
    )
{
    PIRP Irp = Ctx->Irp;
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PHPFS_FCB Fcb = (PHPFS_FCB)FileObject->FsContext;
    PHPFS_CCB Ccb = (PHPFS_CCB)FileObject->FsContext2;
    ULONG ByteCount = IrpSp->Parameters.Read.Length;

    if (Fcb == NULL) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (IrpSp->MinorFunction & IRP_MN_COMPLETE) {
        CcMdlReadComplete(FileObject, Irp->MdlAddress);
        Irp->MdlAddress = NULL;
        return STATUS_SUCCESS;
    }

    Irp->IoStatus.Information = 0;

    if (ByteCount == 0) {
        return STATUS_SUCCESS;
    }

    if (HpfsIsVfcb(Fcb)) {
        return HpfsReadVolume(Ctx, Fcb, Ccb, IrpSp->Parameters.Read.ByteOffset.QuadPart, ByteCount);
    }

    if (!HpfsIsFcb(Fcb)) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    return HpfsReadFile(Ctx, Fcb, IrpSp->Parameters.Read.ByteOffset, ByteCount);
}
