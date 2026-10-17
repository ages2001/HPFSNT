/*
 * HPFSNT - IRP_MJ_WRITE
 *
 * ValidDataLength (VDL) is kept as HPFS keeps it on the disk: the file
 * reads as zeros from VDL to FileSize, so extending a file costs nothing
 * until data is written past VDL. Then the gap is zeroed first, through
 * the cache for cached writes and on the disk otherwise. The entry set
 * gets the new sizes when the handle is cleaned up or the file flushed.
 *
 * Only paging writes that the lazy writer did not issue may move VDL
 * forward (mapped files, flushes we start ourselves); whoever issues them
 * holds the file exclusive.
 */

#include "hpfs.h"

/* Cache Manager calls, run through HpfsTryCall (the Cache Manager raises) */
typedef struct _HPFS_CC_WRITE {
    PFILE_OBJECT FileObject;
    PHPFS_FCB   Fcb;
    PIRP        Irp;
    BOOLEAN     InitOnly;           /* only set up caching */
    BOOLEAN     Mdl;
    LARGE_INTEGER Start;
    ULONG       Count;
    PVOID       Buffer;
    NTSTATUS    Status;
} HPFS_CC_WRITE;

static VOID
HpfsCcWriteBody (
    PVOID Context
    )
{
    HPFS_CC_WRITE *C = (HPFS_CC_WRITE *)Context;

    if (C->Fcb != NULL && C->FileObject->PrivateCacheMap == NULL) {
        CcInitializeCacheMap(C->FileObject, (PCC_FILE_SIZES)&C->Fcb->Header.AllocationSize, FALSE,
                             &HpfsData.CacheManagerCallbacks, C->Fcb);
    }
    if (C->InitOnly) {
        return;
    }
    if (C->Mdl) {
        CcPrepareMdlWrite(C->FileObject, &C->Start, C->Count, &C->Irp->MdlAddress, &C->Irp->IoStatus);
        C->Status = C->Irp->IoStatus.Status;
    } else if (!CcCopyWrite(C->FileObject, &C->Start, C->Count, TRUE, C->Buffer)) {
        C->Status = STATUS_UNSUCCESSFUL;
    }
}

typedef struct _HPFS_CC_ZERO {
    PFILE_OBJECT FileObject;
    LARGE_INTEGER From;
    LARGE_INTEGER To;
} HPFS_CC_ZERO;

static VOID
HpfsCcZeroBody (
    PVOID Context
    )
{
    HPFS_CC_ZERO *C = (HPFS_CC_ZERO *)Context;
    (VOID)CcZeroData(C->FileObject, &C->From, &C->To, TRUE);
}

#ifndef FSRTL_FLAG_USER_MAPPED_FILE
#define FSRTL_FLAG_USER_MAPPED_FILE     0x20
#endif

/*
 * Zeroes [Start, End) of a file whose data up to Start is valid. Through
 * the cache when this file object caches the file, else on the disk.
 */
/* Writes Length zero bytes at Offset through the cache */
static NTSTATUS
HpfsCopyZeros (
    PFILE_OBJECT FileObject,
    LONGLONG Offset,
    ULONG Length
    )
{
    LARGE_INTEGER From;
    PVOID Zero;

    Zero = ExAllocatePoolWithTag(PagedPool, Length, HPFS_TAG_BUFFER);
    if (Zero == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(Zero, Length);
    From.QuadPart = Offset;

    {
        HPFS_CC_WRITE Cc;
        NTSTATUS Code;

        RtlZeroMemory(&Cc, sizeof(Cc));
        Cc.FileObject = FileObject;
        Cc.Start = From;
        Cc.Count = Length;
        Cc.Buffer = Zero;
        Code = HpfsTryCall(HpfsCcWriteBody, &Cc, FALSE);

        ExFreePool(Zero);
        return NT_SUCCESS(Code) ? STATUS_SUCCESS : HpfsExceptionStatus(Code);
    }
}

/*
 * Zeroes [Start, End) of a file whose data up to Start is valid; End is
 * within FileSize. Through the cache when this file object caches the
 * file (CcZeroData for the whole sectors, copies for the ends), else on
 * the disk.
 */
NTSTATUS
HpfsZeroFileRange (
    PHPFS_FCB Fcb,
    PFILE_OBJECT FileObject,
    LONGLONG Start,
    LONGLONG End
    )
{
    PHPFS_VCB Vcb = Fcb->Vcb;
    LARGE_INTEGER From;
    LARGE_INTEGER To;
    LONGLONG HeadEnd;
    LONGLONG TailStart;
    NTSTATUS Status;

    if (Start >= End) {
        return STATUS_SUCCESS;
    }

    if (FileObject == NULL || FileObject->PrivateCacheMap == NULL) {
        return HpfsZeroDisk(Vcb, Fcb, Start, End);
    }

    HeadEnd = HpfsRoundUp(Start, HPFS_SECTOR_SIZE);
    TailStart = End & ~(LONGLONG)(HPFS_SECTOR_SIZE - 1);

    if (HeadEnd >= TailStart) {
        /* Within one or two sectors: just copy zeros */
        return HpfsCopyZeros(FileObject, Start, (ULONG)(End - Start));
    }

    if (Start < HeadEnd) {
        Status = HpfsCopyZeros(FileObject, Start, (ULONG)(HeadEnd - Start));
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }

    From.QuadPart = HeadEnd;
    To.QuadPart = TailStart;

    {
        HPFS_CC_ZERO Cz;
        NTSTATUS Code;

        Cz.FileObject = FileObject;
        Cz.From = From;
        Cz.To = To;
        Code = HpfsTryCall(HpfsCcZeroBody, &Cz, FALSE);
        if (!NT_SUCCESS(Code)) {
            return HpfsExceptionStatus(Code);
        }
    }

    if (TailStart < End) {
        return HpfsCopyZeros(FileObject, TailStart, (ULONG)(End - TailStart));
    }

    return STATUS_SUCCESS;
}

/* DASD writes, only through a handle that locked or dismounted the volume */
static NTSTATUS
HpfsWriteVolume (
    PHPFS_IRP_CONTEXT Ctx,
    PHPFS_FCB Vfcb,
    PHPFS_CCB Ccb,
    LONGLONG StartingByte,
    ULONG ByteCount
    )
{
    PIRP Irp = Ctx->Irp;
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PHPFS_VCB Vcb = Vfcb->Vcb;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Ccb == NULL || (Irp->Flags & IRP_PAGING_IO)) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        if (Vcb->LockFileObject != FileObject && !(Ccb->Flags & CCB_FLAG_DISMOUNTED_VOLUME)) {
            Status = STATUS_ACCESS_DENIED;
            __leave;
        }

        if (((ULONG)StartingByte & (HPFS_SECTOR_SIZE - 1)) != 0 ||
            (ByteCount & (HPFS_SECTOR_SIZE - 1)) != 0) {

            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        if (StartingByte >= Vcb->PartitionBytes) {
            Status = STATUS_END_OF_FILE;
            __leave;
        }

        if (StartingByte + ByteCount > Vcb->PartitionBytes) {
            ByteCount = (ULONG)(Vcb->PartitionBytes - StartingByte);
        }

        Vcb->VcbState |= VCB_STATE_DASD_WRITTEN;

        /* The engine's cached view of the disk is about to go stale */
        HpfsEngineEnter(Vcb);
        (VOID)HpfsMetaFlush(Vcb);
        HpfsMetaPurge(Vcb);
        HpfsRelease(&Vcb->EngineResource);

        Status = HpfsLockUserBuffer(Irp, IoReadAccess, ByteCount);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        Status = HpfsNonCachedIo(Ctx, Vfcb, IRP_MJ_WRITE, StartingByte, ByteCount, Vcb->PartitionBytes);

        if (NT_SUCCESS(Status)) {

            Irp->IoStatus.Information = ByteCount;

            if (FileObject->Flags & FO_SYNCHRONOUS_IO) {
                FileObject->CurrentByteOffset.QuadPart = StartingByte + ByteCount;
            }
        }

    } __finally {

        HpfsRelease(&Vcb->Resource);
    }

    return Status;
}

/* Paging writes of a file: never past FileSize */
static NTSTATUS
HpfsPagingWrite (
    PHPFS_IRP_CONTEXT Ctx,
    PHPFS_FCB Fcb,
    LONGLONG StartingVbo,
    ULONG ByteCount
    )
{
    PHPFS_VCB Vcb = Fcb->Vcb;
    PIRP Irp = Ctx->Irp;
    LONGLONG FileSize;
    LONGLONG ValidData;
    LONGLONG End;
    BOOLEAN ExtendValidData = FALSE;
    NTSTATUS Status = STATUS_SUCCESS;

    (VOID)ExAcquireResourceSharedLite(&Fcb->PagingIoResource, TRUE);

    __try {

        FileSize = Fcb->Header.FileSize.QuadPart;
        ValidData = Fcb->Header.ValidDataLength.QuadPart;

        if (StartingVbo >= FileSize) {
            __leave;
        }

        if (StartingVbo + ByteCount > FileSize) {
            ByteCount = (ULONG)(FileSize - StartingVbo);
        }

        End = StartingVbo + ByteCount;

        /* A paging file is managed entirely by the Memory Manager: never
         * zero-fill or extend valid data for it (doing so from the modified
         * page writer froze the system). Just write the already-allocated run. */
        if (Fcb->FcbState & FCB_STATE_PAGING_FILE) {

            Status = HpfsNonCachedIo(Ctx, Fcb, IRP_MJ_WRITE, StartingVbo,
                                    (ULONG)HpfsRoundUp(ByteCount, HPFS_SECTOR_SIZE), 0);
            if (NT_SUCCESS(Status)) {
                Irp->IoStatus.Information = ByteCount;
            }
            __leave;
        }

        if (End > ValidData) {

            if (Fcb->LazyWriteThread == KeGetCurrentThread()) {

                /* A mapped page past VDL waits for the mapped page writer,
                   which may move VDL */
                if ((Fcb->Header.Flags & FSRTL_FLAG_USER_MAPPED_FILE) &&
                    End > HpfsRoundUp(ValidData, HPFS_PAGE_SIZE)) {

                    Status = STATUS_FILE_LOCK_CONFLICT;
                    __leave;
                }

            } else {

                ExtendValidData = TRUE;

                if (StartingVbo > ValidData) {
                    Status = HpfsZeroDisk(Vcb, Fcb, ValidData, StartingVbo);
                    if (!NT_SUCCESS(Status)) {
                        __leave;
                    }
                }
            }
        }

        Status = HpfsNonCachedIo(Ctx, Fcb, IRP_MJ_WRITE, StartingVbo,
                                (ULONG)HpfsRoundUp(ByteCount, HPFS_SECTOR_SIZE), 0);

        if (NT_SUCCESS(Status)) {

            Irp->IoStatus.Information = ByteCount;

            if (ExtendValidData && End > Fcb->Header.ValidDataLength.QuadPart) {
                Fcb->Header.ValidDataLength.QuadPart = End;
                Fcb->FcbState |= FCB_STATE_DIRENT_DIRTY;
            }
        }

    } __finally {

        HpfsRelease(&Fcb->PagingIoResource);
    }

    return Status;
}

static NTSTATUS
HpfsWriteFile (
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
    BOOLEAN NonCached = (BOOLEAN)((Irp->Flags & IRP_NOCACHE) != 0);
    BOOLEAN WriteToEof;
    BOOLEAN VcbAcquired = FALSE;
    BOOLEAN FcbAcquired = FALSE;
    BOOLEAN PagingAcquired = FALSE;
    BOOLEAN Extended = FALSE;
    LONGLONG OldFileSize = 0;
    LONGLONG OldValidData = 0;
    LONGLONG End;
    IO_STATUS_BLOCK Iosb;
    PVOID Buffer;
    NTSTATUS Status;

    WriteToEof = (BOOLEAN)(StartingByte.LowPart == FILE_WRITE_TO_END_OF_FILE &&
                           StartingByte.HighPart == -1);

    Status = HpfsVerifyWritable(Ctx, Vcb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    if (NonCached &&
        ((!WriteToEof && ((ULONG)StartingByte.QuadPart & (HPFS_SECTOR_SIZE - 1)) != 0) ||
         (ByteCount & (HPFS_SECTOR_SIZE - 1)) != 0)) {

        return STATUS_INVALID_PARAMETER;
    }

    /* Throttle before taking anything the lazy writer may need */
    if (!NonCached) {
        (VOID)CcCanIWrite(FileObject, ByteCount, TRUE, FALSE);
    }

    __try {

        (VOID)ExAcquireResourceSharedLite(&Vcb->Resource, TRUE);
        VcbAcquired = TRUE;

        (VOID)ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);
        FcbAcquired = TRUE;

        Status = HpfsVerifyWritable(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        if (Fcb->FcbState & FCB_STATE_DELETED) {
            Status = STATUS_FILE_DELETED;
            __leave;
        }

        if (WriteToEof) {
            StartingByte = Fcb->Header.FileSize;
        }

        if (NonCached && ((ULONG)StartingByte.QuadPart & (HPFS_SECTOR_SIZE - 1)) != 0) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        /* Keep the cache coherent with what goes straight to the disk */
        if (NonCached && Fcb->SectionObjectPointers.DataSectionObject != NULL) {

            (VOID)ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
            PagingAcquired = TRUE;

            CcFlushCache(&Fcb->SectionObjectPointers, &StartingByte, ByteCount, &Iosb);

            if (!NT_SUCCESS(Iosb.Status)) {
                Status = Iosb.Status;
                __leave;
            }

            (VOID)CcPurgeCacheSection(&Fcb->SectionObjectPointers, &StartingByte, ByteCount, FALSE);
        }

        if (!FsRtlCheckLockForWriteAccess(&Fcb->FileLock, Irp)) {
            Status = STATUS_FILE_LOCK_CONFLICT;
            __leave;
        }

        End = StartingByte.QuadPart + ByteCount;

        if (StartingByte.QuadPart < 0 || End < StartingByte.QuadPart) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        OldFileSize = Fcb->Header.FileSize.QuadPart;
        OldValidData = Fcb->Header.ValidDataLength.QuadPart;

        if (End > OldFileSize) {

            if (End > HPFS_MAX_FILE_SIZE) {
                Status = STATUS_DISK_FULL;
                __leave;
            }

            if (End > Fcb->Header.AllocationSize.QuadPart) {

                Status = HpfsSetAllocation(Vcb, Fcb, (ULONGLONG)End);
                if (!NT_SUCCESS(Status)) {
                    __leave;
                }
            }

            Fcb->Header.FileSize.QuadPart = End;
            Fcb->FcbState |= FCB_STATE_DIRENT_DIRTY;
            Extended = TRUE;

            if (Fcb->SectionObjectPointers.SharedCacheMap != NULL) {
                CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize);
            }
        }

        if (!NonCached) {

            HPFS_CC_WRITE Cc;
            NTSTATUS Code;

            Buffer = NULL;
            if (!(IrpSp->MinorFunction & IRP_MN_MDL)) {
                Buffer = HpfsMapUserBuffer(Irp);
                if (Buffer == NULL) {
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    __leave;
                }
            }

            RtlZeroMemory(&Cc, sizeof(Cc));
            Cc.FileObject = FileObject;
            Cc.Fcb = Fcb;
            Cc.Irp = Irp;
            Cc.InitOnly = TRUE;
            Code = HpfsTryCall(HpfsCcWriteBody, &Cc, FALSE);
            if (!NT_SUCCESS(Code)) {
                Status = HpfsExceptionStatus(Code);
            }

            if (!NT_SUCCESS(Status)) {
                __leave;
            }

            if (StartingByte.QuadPart > OldValidData) {
                Status = HpfsZeroFileRange(Fcb, FileObject, OldValidData, StartingByte.QuadPart);
                if (!NT_SUCCESS(Status)) {
                    __leave;
                }
            }

            Cc.InitOnly = FALSE;
            Cc.Mdl = (BOOLEAN)((IrpSp->MinorFunction & IRP_MN_MDL) != 0);
            Cc.Start = StartingByte;
            Cc.Count = ByteCount;
            Cc.Buffer = Buffer;
            Cc.Status = Status;
            Code = HpfsTryCall(HpfsCcWriteBody, &Cc, FALSE);
            Status = NT_SUCCESS(Code) ? Cc.Status : HpfsExceptionStatus(Code);

            if (!NT_SUCCESS(Status)) {
                __leave;
            }

        } else {

            if (StartingByte.QuadPart > OldValidData) {
                Status = HpfsZeroDisk(Vcb, Fcb, OldValidData, StartingByte.QuadPart);
                if (!NT_SUCCESS(Status)) {
                    __leave;
                }
            }

            Status = HpfsLockUserBuffer(Irp, IoReadAccess, ByteCount);
            if (!NT_SUCCESS(Status)) {
                __leave;
            }

            Status = HpfsNonCachedIo(Ctx, Fcb, IRP_MJ_WRITE, StartingByte.QuadPart, ByteCount, 0);
        }

        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        if (End > Fcb->Header.ValidDataLength.QuadPart) {

            Fcb->Header.ValidDataLength.QuadPart = End;
            Fcb->FcbState |= FCB_STATE_DIRENT_DIRTY;

            /* Tell the cache the disk now holds valid data up to here */
            if (NonCached && Fcb->SectionObjectPointers.SharedCacheMap != NULL) {
                CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize);
            }
        }

        Irp->IoStatus.Information = ByteCount;

        if (FileObject->Flags & FO_SYNCHRONOUS_IO) {
            FileObject->CurrentByteOffset.QuadPart = End;
        }

        FileObject->Flags |= FO_FILE_MODIFIED;

        if (Extended) {
            FileObject->Flags |= FO_FILE_SIZE_CHANGED;
        }

    } __finally {

        if (Extended && (AbnormalTermination() || !NT_SUCCESS(Status))) {

            /* Pull the sizes back; the allocation goes at cleanup */
            if (!PagingAcquired) {
                (VOID)ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
                PagingAcquired = TRUE;
            }

            Fcb->Header.FileSize.QuadPart = OldFileSize;
            Fcb->Header.ValidDataLength.QuadPart = OldValidData;
            Fcb->FcbState |= FCB_STATE_TRUNCATE_ON_CLOSE;

            if (Fcb->SectionObjectPointers.SharedCacheMap != NULL) {
                CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize);
            }
        }

        if (PagingAcquired) {
            HpfsRelease(&Fcb->PagingIoResource);
        }

        if (FcbAcquired) {
            HpfsRelease(&Fcb->Resource);
        }

        if (VcbAcquired) {
            HpfsRelease(&Vcb->Resource);
        }
    }

    return Status;
}

NTSTATUS
HpfsCommonWrite (
    PHPFS_IRP_CONTEXT Ctx
    )
{
    PIRP Irp = Ctx->Irp;
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PHPFS_FCB Fcb = (PHPFS_FCB)FileObject->FsContext;
    PHPFS_CCB Ccb = (PHPFS_CCB)FileObject->FsContext2;
    ULONG ByteCount = IrpSp->Parameters.Write.Length;

    if (Fcb == NULL) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (IrpSp->MinorFunction & IRP_MN_COMPLETE) {
        CcMdlWriteComplete(FileObject, &IrpSp->Parameters.Write.ByteOffset, Irp->MdlAddress);
        Irp->MdlAddress = NULL;
        return STATUS_SUCCESS;
    }

    Irp->IoStatus.Information = 0;

    if (ByteCount == 0) {
        return STATUS_SUCCESS;
    }

    if (HpfsIsVfcb(Fcb)) {
        return HpfsWriteVolume(Ctx, Fcb, Ccb, IrpSp->Parameters.Write.ByteOffset.QuadPart, ByteCount);
    }

    if (!HpfsIsFcb(Fcb)) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (Irp->Flags & IRP_PAGING_IO) {
        return HpfsPagingWrite(Ctx, Fcb, IrpSp->Parameters.Write.ByteOffset.QuadPart, ByteCount);
    }

    return HpfsWriteFile(Ctx, Fcb, IrpSp->Parameters.Write.ByteOffset, ByteCount);
}
