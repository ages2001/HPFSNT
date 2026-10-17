/*
 * HPFSNT - disk I/O
 *
 * Synchronous requests to the target device, sector I/O for the metadata
 * cache (through the hotfix table), caller buffers, and the non-cached
 * transfer of file data: one request per contiguous run of the file's
 * allocation, split where a hotfix replaces a sector.
 */

#include "hpfs.h"

/* ------------------------------------------------------------------ */
/* Synchronous requests to the target device                           */
/* ------------------------------------------------------------------ */

NTSTATUS
HpfsSyncIo (
    PDEVICE_OBJECT Device,
    UCHAR MajorFunction,
    LONGLONG Offset,
    ULONG Length,
    PVOID Buffer,
    BOOLEAN OverrideVerify
    )
{
    KEVENT Event;
    IO_STATUS_BLOCK Iosb;
    LARGE_INTEGER ByteOffset;
    PIRP Irp;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    ByteOffset.QuadPart = Offset;

    Irp = IoBuildSynchronousFsdRequest(MajorFunction, Device, Buffer, Length,
                                       &ByteOffset, &Event, &Iosb);
    if (Irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (OverrideVerify) {
        IoGetNextIrpStackLocation(Irp)->Flags |= SL_OVERRIDE_VERIFY_VOLUME;
    }

    Status = IoCallDriver(Device, Irp);

    if (Status == STATUS_PENDING) {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Iosb.Status;
    }
    return Status;
}

NTSTATUS
HpfsDeviceIoctl (
    PDEVICE_OBJECT Device,
    ULONG IoControlCode,
    PVOID InputBuffer,
    ULONG InputLength,
    PVOID OutputBuffer,
    ULONG OutputLength,
    BOOLEAN OverrideVerify,
    PULONG Information
    )
{
    KEVENT Event;
    IO_STATUS_BLOCK Iosb;
    PIRP Irp;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    Irp = IoBuildDeviceIoControlRequest(IoControlCode, Device, InputBuffer, InputLength,
                                        OutputBuffer, OutputLength, FALSE, &Event, &Iosb);
    if (Irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (OverrideVerify) {
        IoGetNextIrpStackLocation(Irp)->Flags |= SL_OVERRIDE_VERIFY_VOLUME;
    }

    Status = IoCallDriver(Device, Irp);

    if (Status == STATUS_PENDING) {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Iosb.Status;
    }
    if (Information != NULL) {
        *Information = (ULONG)Iosb.Information;
    }
    return Status;
}

NTSTATUS
HpfsFlushDevice (
    PHPFS_VCB Vcb
    )
{
    KEVENT Event;
    IO_STATUS_BLOCK Iosb;
    PIRP Irp;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_FLUSH_BUFFERS, Vcb->TargetDeviceObject,
                                       NULL, 0, NULL, &Event, &Iosb);
    if (Irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = IoCallDriver(Vcb->TargetDeviceObject, Irp);

    if (Status == STATUS_PENDING) {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Iosb.Status;
    }
    return Status;
}

/* ------------------------------------------------------------------ */
/* Sector I/O through the hotfix table                                 */
/* ------------------------------------------------------------------ */

/*
 * Of Count sectors from Lsn: how many can go to the disk as one run, and
 * where. A hotfixed sector goes alone, to its replacement.
 */
static ULONG
HpfsHotfixRun (
    PHPFS_VCB Vcb,
    ULONG Lsn,
    ULONG Count,
    PULONG Target
    )
{
    PHPFS_VOL Vol = &Vcb->Vol;
    ULONG i, Bad, Run = Count;

    *Target = Lsn;
    for (i = 0; i < Vol->HotfixCount; i++) {
        Bad = Vol->HotfixBad[i];
        if (Bad == Lsn) {
            *Target = Vol->HotfixSpare[i];
            return 1;
        }
        if (Bad > Lsn && Bad - Lsn < Run) {
            Run = Bad - Lsn;
        }
    }
    return Run;
}

static NTSTATUS
HpfsSectorIo (
    PHPFS_VCB Vcb,
    UCHAR Major,
    ULONG Sector,
    ULONG Count,
    PUCHAR Buffer
    )
{
    ULONG Target, Run;
    NTSTATUS Status;
    BOOLEAN Override = (BOOLEAN)(Vcb->VerifyThread == KeGetCurrentThread());

    while (Count != 0) {
        Run = HpfsHotfixRun(Vcb, Sector, Count, &Target);
        Status = HpfsSyncIo(Vcb->TargetDeviceObject, Major,
                            (LONGLONG)Target << HPFS_SECTOR_SHIFT,
                            Run << HPFS_SECTOR_SHIFT, Buffer, Override);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
        Sector += Run;
        Count -= Run;
        Buffer += Run << HPFS_SECTOR_SHIFT;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
HpfsReadDisk (
    PHPFS_VCB Vcb,
    ULONG Sector,
    ULONG Count,
    PVOID Buffer
    )
{
    return HpfsSectorIo(Vcb, IRP_MJ_READ, Sector, Count, (PUCHAR)Buffer);
}

NTSTATUS
HpfsWriteDisk (
    PHPFS_VCB Vcb,
    ULONG Sector,
    ULONG Count,
    PVOID Buffer
    )
{
    if (Vcb->VcbState & VCB_STATE_READ_ONLY) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }
    return HpfsSectorIo(Vcb, IRP_MJ_WRITE, Sector, Count, (PUCHAR)Buffer);
}

/* ------------------------------------------------------------------ */
/* Caller buffers                                                      */
/* ------------------------------------------------------------------ */

typedef struct _HPFS_PROBE {
    PMDL        Mdl;
    KPROCESSOR_MODE Mode;
    LOCK_OPERATION Operation;
} HPFS_PROBE;

static VOID
HpfsProbeBody (
    PVOID Context
    )
{
    HPFS_PROBE *P = (HPFS_PROBE *)Context;
    MmProbeAndLockPages(P->Mdl, P->Mode, P->Operation);
}

NTSTATUS
HpfsLockUserBuffer (
    PIRP Irp,
    LOCK_OPERATION Operation,
    ULONG Length
    )
{
    PMDL Mdl;
    HPFS_PROBE Probe;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Irp->MdlAddress != NULL) {
        return STATUS_SUCCESS;
    }

    Mdl = IoAllocateMdl(Irp->UserBuffer, Length, FALSE, FALSE, Irp);
    if (Mdl == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Probe.Mdl = Mdl;
    Probe.Mode = Irp->RequestorMode;
    Probe.Operation = Operation;
    if (!NT_SUCCESS(HpfsTryCall(HpfsProbeBody, &Probe, FALSE))) {
        Status = STATUS_INVALID_USER_BUFFER;
    }

    if (!NT_SUCCESS(Status)) {
        IoFreeMdl(Mdl);
        Irp->MdlAddress = NULL;
    }
    return Status;
}

PVOID
HpfsMapUserBuffer (
    PIRP Irp
    )
{
    if (Irp->MdlAddress == NULL) {
        return Irp->UserBuffer;
    }
    return HpfsMdlAddress(Irp->MdlAddress);
}

/* ------------------------------------------------------------------ */
/* File offsets to disk offsets                                        */
/* ------------------------------------------------------------------ */

/*
 * Maps byte Vbo of a file to its volume byte offset and the number of
 * bytes that follow contiguously (before any hotfix split). The caller
 * holds the file's paging resource (or more), so the run list is stable.
 */
BOOLEAN
HpfsVboToLbo (
    PHPFS_VCB Vcb,
    PHPFS_FCB Fcb,
    LONGLONG Vbo,
    PLONGLONG Lbo,
    PULONG Contiguous
    )
{
    ULONG Vsn, Lo, Hi, Mid, Within;
    PHPFS_RUN R;
    LONGLONG Left;

    UNREFERENCED_PARAMETER(Vcb);

    if (HpfsIsVfcb(Fcb)) {
        Left = Fcb->Header.AllocationSize.QuadPart - Vbo;
        if (Vbo < 0 || Left <= 0) {
            return FALSE;
        }
        *Lbo = Vbo;
        *Contiguous = (Left > 0x40000000) ? 0x40000000 : (ULONG)Left;
        return TRUE;
    }

    if (Vbo < 0 || (Vbo >> HPFS_SECTOR_SHIFT) >= Fcb->AllocSectors || Fcb->RunCount == 0) {
        return FALSE;
    }
    Vsn = (ULONG)(Vbo >> HPFS_SECTOR_SHIFT);

    Lo = 0;
    Hi = Fcb->RunCount;
    while (Lo + 1 < Hi) {
        Mid = (Lo + Hi) / 2;
        if (Fcb->Runs[Mid].FileSector <= Vsn) {
            Lo = Mid;
        } else {
            Hi = Mid;
        }
    }
    R = &Fcb->Runs[Lo];
    if (Vsn < R->FileSector || Vsn - R->FileSector >= R->Length) {
        return FALSE;
    }
    Within = Vsn - R->FileSector;
    *Lbo = ((LONGLONG)(R->DiskSector + Within) << HPFS_SECTOR_SHIFT) + (Vbo & (HPFS_SECTOR_SIZE - 1));
    Left = ((LONGLONG)(R->Length - Within) << HPFS_SECTOR_SHIFT) - (Vbo & (HPFS_SECTOR_SIZE - 1));
    *Contiguous = (Left > 0x40000000) ? 0x40000000 : (ULONG)Left;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Non-cached transfers                                                */
/* ------------------------------------------------------------------ */

static NTSTATUS
NTAPI
HpfsSyncCompletion (
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp,
    PVOID Context
    )
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* Moves Length bytes between the disk at Lbo and the master MDL from BufferOffset */
static NTSTATUS
HpfsTransferRun (
    PHPFS_VCB Vcb,
    UCHAR MajorFunction,
    PIRP MasterIrp,
    ULONG BufferOffset,
    LONGLONG Lbo,
    ULONG Length
    )
{
    PMDL MasterMdl = MasterIrp->MdlAddress;
    PMDL Mdl = MasterMdl;
    PUCHAR Va;
    PIRP Irp;
    PIO_STACK_LOCATION IrpSp;
    KEVENT Event;
    NTSTATUS Status;

    Irp = IoAllocateIrp(Vcb->TargetDeviceObject->StackSize, FALSE);
    if (Irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (BufferOffset != 0 || Length != MmGetMdlByteCount(MasterMdl)) {

        Va = (PUCHAR)MmGetMdlVirtualAddress(MasterMdl) + BufferOffset;

        Mdl = IoAllocateMdl(Va, Length, FALSE, FALSE, NULL);
        if (Mdl == NULL) {
            IoFreeIrp(Irp);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        IoBuildPartialMdl(MasterMdl, Mdl, Va, Length);
    }

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    Irp->MdlAddress = Mdl;
    Irp->Flags = IRP_NOCACHE | (MasterIrp->Flags & IRP_PAGING_IO);
    Irp->RequestorMode = KernelMode;
    Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    Irp->UserIosb = NULL;

    IrpSp = IoGetNextIrpStackLocation(Irp);
    IrpSp->MajorFunction = MajorFunction;
    IrpSp->Parameters.Read.Length = Length;
    IrpSp->Parameters.Read.ByteOffset.QuadPart = Lbo;

    if (Vcb->VerifyThread == KeGetCurrentThread()) {
        IrpSp->Flags |= SL_OVERRIDE_VERIFY_VOLUME;
    }

    IoSetCompletionRoutine(Irp, HpfsSyncCompletion, &Event, TRUE, TRUE, TRUE);

    Status = IoCallDriver(Vcb->TargetDeviceObject, Irp);

    if (Status == STATUS_PENDING) {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    }

    Status = Irp->IoStatus.Status;

    if (NT_SUCCESS(Status) && Irp->IoStatus.Information != Length) {
        Status = STATUS_UNEXPECTED_IO_ERROR;
    }

    if (Mdl != MasterMdl) {
        IoFreeMdl(Mdl);
    }

    Irp->MdlAddress = NULL;
    IoFreeIrp(Irp);

    return Status;
}

/*
 * Moves ByteCount bytes (whole sectors) between the disk and Irp->MdlAddress,
 * from StartingVbo of Fcb. Reads return zeros from ValidData on.
 */
NTSTATUS
HpfsNonCachedIo (
    PHPFS_IRP_CONTEXT Ctx,
    PHPFS_FCB Fcb,
    UCHAR MajorFunction,
    LONGLONG StartingVbo,
    ULONG ByteCount,
    LONGLONG ValidData
    )
{
    PHPFS_VCB Vcb = Fcb->Vcb;
    PIRP Irp = Ctx->Irp;
    LONGLONG TransferEnd;
    LONGLONG Vbo;
    LONGLONG Lbo;
    ULONG Contiguous;
    ULONG Offset;
    ULONG Length;
    ULONG ZeroOffset;
    ULONG Target, Run;
    PUCHAR Buffer;
    NTSTATUS Status = STATUS_SUCCESS;

    TransferEnd = StartingVbo + ByteCount;

    if (MajorFunction == IRP_MJ_READ && TransferEnd > ValidData) {

        TransferEnd = HpfsRoundUp(ValidData, HPFS_SECTOR_SIZE);

        if (TransferEnd < StartingVbo) {
            TransferEnd = StartingVbo;
        }
        if (TransferEnd > StartingVbo + ByteCount) {
            TransferEnd = StartingVbo + ByteCount;
        }
    }

    Vbo = StartingVbo;
    Offset = 0;

    while (Vbo < TransferEnd) {

        if (!HpfsVboToLbo(Vcb, Fcb, Vbo, &Lbo, &Contiguous)) {
            HPFS_DBG((HPFS_PFX "No sector for offset %lX of fnode %lX\n", (ULONG)Vbo, Fcb->Fnode));
            return STATUS_FILE_CORRUPT_ERROR;
        }

        Length = (ULONG)(TransferEnd - Vbo);
        if (Length > Contiguous) {
            Length = Contiguous;
        }

        /* Hotfixes: a replaced sector goes alone, to its spare */
        if (Vcb->Vol.HotfixCount != 0 && !HpfsIsVfcb(Fcb)) {
            Run = HpfsHotfixRun(Vcb, (ULONG)(Lbo >> HPFS_SECTOR_SHIFT),
                                (Length + HPFS_SECTOR_SIZE - 1) >> HPFS_SECTOR_SHIFT, &Target);
            if (Target != (ULONG)(Lbo >> HPFS_SECTOR_SHIFT)) {
                Lbo = (LONGLONG)Target << HPFS_SECTOR_SHIFT;
            }
            if (Length > (Run << HPFS_SECTOR_SHIFT)) {
                Length = Run << HPFS_SECTOR_SHIFT;
            }
        }

        Status = HpfsTransferRun(Vcb, MajorFunction, Irp, Offset, Lbo, Length);

        if (!NT_SUCCESS(Status)) {
            HPFS_DBG((HPFS_PFX "Disk transfer at %lX failed %lX\n", (ULONG)Lbo, Status));
            return Status;
        }

        Vbo += Length;
        Offset += Length;
    }

    if (MajorFunction == IRP_MJ_READ && StartingVbo + ByteCount > ValidData) {

        if (ValidData > StartingVbo) {
            ZeroOffset = (ULONG)(ValidData - StartingVbo);
        } else {
            ZeroOffset = 0;
        }

        Buffer = (PUCHAR)HpfsMdlAddress(Irp->MdlAddress);
        if (Buffer == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlZeroMemory(Buffer + ZeroOffset, ByteCount - ZeroOffset);
    }

    return Status;
}

/*
 * Writes zeros over [Start, End) of Fcb on the disk; a partial first
 * sector keeps its bytes before Start. End is rounded up to a sector and
 * clipped to the allocation.
 */
NTSTATUS
HpfsZeroDisk (
    PHPFS_VCB Vcb,
    PHPFS_FCB Fcb,
    LONGLONG Start,
    LONGLONG End
    )
{
    PUCHAR Zero;
    LONGLONG Vbo;
    LONGLONG Lbo;
    ULONG Contiguous;
    ULONG Length;
    ULONG Within;
    NTSTATUS Status = STATUS_SUCCESS;

    End = HpfsRoundUp(End, HPFS_SECTOR_SIZE);
    if (End > Fcb->Header.AllocationSize.QuadPart) {
        End = Fcb->Header.AllocationSize.QuadPart;
    }
    if (Start >= End) {
        return STATUS_SUCCESS;
    }

    Zero = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, HPFS_ZERO_CHUNK, HPFS_TAG_BUFFER);
    if (Zero == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Vbo = Start & ~(LONGLONG)(HPFS_SECTOR_SIZE - 1);
    Within = (ULONG)(Start - Vbo);

    if (Within != 0) {
        if (!HpfsVboToLbo(Vcb, Fcb, Vbo, &Lbo, &Contiguous)) {
            Status = STATUS_FILE_CORRUPT_ERROR;
            goto Done;
        }
        Status = HpfsReadDisk(Vcb, (ULONG)(Lbo >> HPFS_SECTOR_SHIFT), 1, Zero);
        if (!NT_SUCCESS(Status)) goto Done;
        RtlZeroMemory(Zero + Within, HPFS_SECTOR_SIZE - Within);
        Status = HpfsWriteDisk(Vcb, (ULONG)(Lbo >> HPFS_SECTOR_SHIFT), 1, Zero);
        if (!NT_SUCCESS(Status)) goto Done;
        Vbo += HPFS_SECTOR_SIZE;
    }

    RtlZeroMemory(Zero, HPFS_ZERO_CHUNK);

    while (Vbo < End) {
        if (!HpfsVboToLbo(Vcb, Fcb, Vbo, &Lbo, &Contiguous)) {
            Status = STATUS_FILE_CORRUPT_ERROR;
            goto Done;
        }
        Length = HPFS_ZERO_CHUNK;
        if (Length > Contiguous) {
            Length = Contiguous;
        }
        if ((LONGLONG)Length > End - Vbo) {
            Length = (ULONG)(End - Vbo);
        }
        Status = HpfsWriteDisk(Vcb, (ULONG)(Lbo >> HPFS_SECTOR_SHIFT), Length >> HPFS_SECTOR_SHIFT, Zero);
        if (!NT_SUCCESS(Status)) goto Done;
        Vbo += Length;
    }

Done:
    ExFreePool(Zero);
    return Status;
}
