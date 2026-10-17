/*
 * HPFSNT - IRP_MJ_FILE_SYSTEM_CONTROL (mount, verify, lock/unlock,
 * dismount, queries) and IRP_MJ_PNP
 */

#include "hpfs.h"
#include "hpfsctl.h"

/* ------------------------------------------------------------------ */
/* Mount                                                               */
/* ------------------------------------------------------------------ */

/* Hot-plug devices (W2K) get the removable-media flush policy */
#define HPFS_IOCTL_GET_HOTPLUG_INFO CTL_CODE(0x0000002d, 0x0305, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _HPFS_HOTPLUG_INFO {
    ULONG       Size;
    BOOLEAN     MediaRemovable;
    BOOLEAN     MediaHotplug;
    BOOLEAN     DeviceHotplug;
    BOOLEAN     WriteCacheEnableOverride;
} HPFS_HOTPLUG_INFO;

static VOID
HpfsCheckDeviceState (
    PHPFS_VCB Vcb,
    PDEVICE_OBJECT TargetDevice
    )
{
    HPFS_HOTPLUG_INFO Hotplug;
    NTSTATUS Status;

    Status = HpfsDeviceIoctl(TargetDevice, IOCTL_DISK_IS_WRITABLE, NULL, 0, NULL, 0, TRUE, NULL);

    if (Status == STATUS_MEDIA_WRITE_PROTECTED || !HpfsWriteSupportEnabled()) {
        Vcb->VcbState |= VCB_STATE_READ_ONLY;
    }

    if (Vcb->VcbState & VCB_STATE_REMOVABLE) {
        Vcb->VcbState |= VCB_STATE_FLUSH_ON_CLOSE;
    }

    RtlZeroMemory(&Hotplug, sizeof(Hotplug));

    Status = HpfsDeviceIoctl(TargetDevice, HPFS_IOCTL_GET_HOTPLUG_INFO, NULL, 0,
                             &Hotplug, sizeof(Hotplug), TRUE, NULL);

    if (NT_SUCCESS(Status) && (Hotplug.MediaRemovable || Hotplug.DeviceHotplug)) {
        Vcb->VcbState |= VCB_STATE_FLUSH_ON_CLOSE;
    }
}

/* The label as NT shows it: the boot sector's, in the OEM code page */
VOID
HpfsLoadLabel (
    PHPFS_VCB Vcb
    )
{
    WCHAR Wide[HPFS_MAX_LABEL];
    ULONG Bytes = 0;
    ULONG i;
    PVPB Vpb = Vcb->Vpb;

    Vcb->LabelLength = 0;
    if (Vcb->Vol.LabelLength != 0 &&
        NT_SUCCESS(RtlOemToUnicodeN(Wide, sizeof(Wide), &Bytes, (PCHAR)Vcb->Vol.Label,
                                    Vcb->Vol.LabelLength))) {
        Vcb->LabelLength = Bytes / sizeof(WCHAR);
        RtlCopyMemory(Vcb->Label, Wide, Bytes);
    }

    Vpb->VolumeLabelLength = (USHORT)(Vcb->LabelLength * sizeof(WCHAR));
    for (i = 0; i < Vcb->LabelLength; i++) {
        Vpb->VolumeLabel[i] = Vcb->Label[i];
    }
}

/* The root has no entry of its own; its ".." entry carries its times */
static VOID
HpfsLoadRootTimes (
    PHPFS_VCB Vcb
    )
{
    PHPFS_ENTRY Entry;
    NTSTATUS Status;

    Entry = (PHPFS_ENTRY)ExAllocatePoolWithTag(PagedPool, sizeof(HPFS_ENTRY), HPFS_TAG_BUFFER);
    if (Entry == NULL) {
        return;
    }
    HpfsEngineEnter(Vcb);
    Status = HpfsEngineLeave(Vcb, HpfsGetDotDot(&Vcb->Vol, Vcb->Vol.RootFnode, Entry));
    if (NT_SUCCESS(Status)) {
        Vcb->RootDcb->CreationTime = HpfsToNtTime(Entry->CreateTime);
        Vcb->RootDcb->LastWriteTime = HpfsToNtTime(Entry->WriteTime);
        Vcb->RootDcb->LastAccessTime = HpfsToNtTime(Entry->AccessTime);
    }
    ExFreePool(Entry);
}

/*
 * (Re)reads the engine's view of the volume: superblock, spareblock,
 * bitmap list, code page, hotfixes. Vcb->Resource exclusive.
 */
NTSTATUS
HpfsLoadVolume (
    PHPFS_VCB Vcb
    )
{
    BOOLEAN ReadOnly = (BOOLEAN)((Vcb->VcbState & VCB_STATE_READ_ONLY) != 0);
    ULONG PartitionSectors = (ULONG)(Vcb->PartitionBytes >> HPFS_SECTOR_SHIFT);
    NTSTATUS Status;

    HpfsEngineEnter(Vcb);

    HpfsMetaPurge(Vcb);
    HpfsUnmountFree(&Vcb->Vol);
    Vcb->Vol.HostContext = Vcb;
    Vcb->Vol.HotfixCount = 0;
    Vcb->Vol.TotalSectors = PartitionSectors ? PartitionSectors : 18;

    Status = HpfsMount(&Vcb->Vol, ReadOnly);

    if (NT_SUCCESS(Status) && PartitionSectors != 0 && Vcb->Vol.TotalSectors > PartitionSectors) {
        HPFS_DBG((HPFS_PFX "file system (%lu sectors) larger than the partition (%lu)\n",
                  Vcb->Vol.TotalSectors, PartitionSectors));
        Status = STATUS_DISK_CORRUPT_ERROR;
    }

    if (NT_SUCCESS(Status)) {
        if (Vcb->Vol.SpareFlags & HPFS_SPARE_DIRTY) {
            /* Left dirty (not by us): keep it so for the check tool */
            if (!Vcb->DirtyMarked) {
                Vcb->VcbState |= VCB_STATE_KEEP_DIRTY;
            }
            Vcb->DirtyMarked = TRUE;
            Vcb->Vol.MarkedDirty = TRUE;
        } else {
            /* Clean on the disk (a check tool may have repaired it) */
            Vcb->DirtyMarked = FALSE;
            Vcb->VcbState &= ~VCB_STATE_KEEP_DIRTY;
        }
        Vcb->VolumeBytes = (LONGLONG)Vcb->Vol.TotalSectors << HPFS_SECTOR_SHIFT;
        if (Vcb->PartitionBytes < Vcb->VolumeBytes) {
            Vcb->PartitionBytes = Vcb->VolumeBytes;
        }
        Vcb->SerialNumber = Vcb->Vol.SerialNumber;
        if (ReadOnly) {
            Vcb->Vol.ReadOnly = TRUE;
        }
    } else {
        HpfsUnmountFree(&Vcb->Vol);
    }

    HpfsMetaPurge(Vcb);
    HpfsRelease(&Vcb->EngineResource);

    if (Status == HPFS_UNRECOGNIZED) {
        Status = STATUS_UNRECOGNIZED_VOLUME;
    }
    return Status;
}

static NTSTATUS
HpfsMountVolume (
    PHPFS_IRP_CONTEXT Ctx
    )
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PDEVICE_OBJECT TargetDevice = IrpSp->Parameters.MountVolume.DeviceObject;
    PVPB Vpb = IrpSp->Parameters.MountVolume.Vpb;
    PDEVICE_OBJECT RealDevice = Vpb->RealDevice;
    DISK_GEOMETRY Geometry;
    PARTITION_INFORMATION Partition;
    PDEVICE_OBJECT VolumeDevice = NULL;
    PHPFS_VCB Vcb = NULL;
    PUCHAR Sector = NULL;
    LONGLONG PartitionBytes;
    BOOLEAN ClearedVerify = FALSE;
    BOOLEAN Resources = FALSE;
    NTSTATUS Status;

    Status = HpfsDeviceIoctl(TargetDevice, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0,
                             &Geometry, sizeof(Geometry), TRUE, NULL);
    if (!NT_SUCCESS(Status) || Geometry.BytesPerSector != HPFS_SECTOR_SIZE) {
        return STATUS_UNRECOGNIZED_VOLUME;
    }

    Status = HpfsDeviceIoctl(TargetDevice, IOCTL_DISK_GET_PARTITION_INFO, NULL, 0,
                             &Partition, sizeof(Partition), TRUE, NULL);
    PartitionBytes = NT_SUCCESS(Status) ? Partition.PartitionLength.QuadPart : 0;
    if (PartitionBytes == 0) {
        /* Floppies and superfloppies: the geometry gives the size */
        PartitionBytes = Geometry.Cylinders.QuadPart * Geometry.TracksPerCylinder *
                         Geometry.SectorsPerTrack * HPFS_SECTOR_SIZE;
    }
    if (PartitionBytes > ((LONGLONG)HPFS_MAX_SECTORS << HPFS_SECTOR_SHIFT)) {
        PartitionBytes = (LONGLONG)HPFS_MAX_SECTORS << HPFS_SECTOR_SHIFT;
    }

    Sector = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, HPFS_SECTOR_SIZE, HPFS_TAG_BUFFER);
    if (Sector == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    __try {

        /* Cheap test first: most volumes offered to us are not HPFS */
        Status = HpfsReadSectors(TargetDevice, 0, HPFS_SECTOR_SIZE, Sector, TRUE);
        if (!NT_SUCCESS(Status) || !HpfsIsBootSector(Sector)) {
            Status = STATUS_UNRECOGNIZED_VOLUME;
            __leave;
        }

        Status = IoCreateDevice(HpfsData.DriverObject,
                                sizeof(HPFS_VCB),
                                NULL,
                                FILE_DEVICE_DISK_FILE_SYSTEM,
                                0,
                                FALSE,
                                &VolumeDevice);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        VolumeDevice->StackSize = (CCHAR)(TargetDevice->StackSize + 1);

        if (TargetDevice->AlignmentRequirement > VolumeDevice->AlignmentRequirement) {
            VolumeDevice->AlignmentRequirement = TargetDevice->AlignmentRequirement;
        }

        VolumeDevice->SectorSize = HPFS_SECTOR_SIZE;

#ifdef DO_DEVICE_INITIALIZING
        VolumeDevice->Flags &= ~DO_DEVICE_INITIALIZING;
#endif

        Vcb = (PHPFS_VCB)VolumeDevice->DeviceExtension;
        RtlZeroMemory(Vcb, sizeof(HPFS_VCB));

        Vcb->NodeTypeCode = HPFS_NTC_VCB;
        Vcb->NodeByteSize = (CSHORT)sizeof(HPFS_VCB);
        ExInitializeResourceLite(&Vcb->Resource);
        ExInitializeResourceLite(&Vcb->EngineResource);
        Resources = TRUE;
        InitializeListHead(&Vcb->FcbList);
        InitializeListHead(&Vcb->DirNotifyList);
        InitializeListHead(&Vcb->VcbLinks);
        FsRtlNotifyInitializeSync(&Vcb->NotifySync);
        HpfsMetaInitialize(Vcb);

        Vcb->Vpb = Vpb;
        Vcb->TargetDeviceObject = TargetDevice;
        Vcb->VolumeDeviceObject = VolumeDevice;
        Vcb->VerifyThread = KeGetCurrentThread();
        Vcb->PartitionBytes = PartitionBytes;

        if (RealDevice->Characteristics & FILE_REMOVABLE_MEDIA) {
            Vcb->VcbState |= VCB_STATE_REMOVABLE;
        }

        HpfsCheckDeviceState(Vcb, TargetDevice);

        if (RealDevice->Flags & DO_VERIFY_VOLUME) {
            RealDevice->Flags &= ~DO_VERIFY_VOLUME;
            ClearedVerify = TRUE;
        }

        Status = HpfsLoadVolume(Vcb);
        if (!NT_SUCCESS(Status)) {
            HPFS_DBG((HPFS_PFX "mount failed %lX\n", Status));
            if (Status == STATUS_UNRECOGNIZED_VOLUME || Status == STATUS_DISK_CORRUPT_ERROR ||
                Status == STATUS_FILE_CORRUPT_ERROR) {
                Status = STATUS_UNRECOGNIZED_VOLUME;
            }
            __leave;
        }

        Vcb->RootDcb = HpfsCreateRootDcb(Vcb);
        Vcb->VolumeFcb = HpfsCreateVolumeFcb(Vcb);
        if (Vcb->RootDcb == NULL || Vcb->VolumeFcb == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        HpfsLoadLabel(Vcb);
        HpfsLoadRootTimes(Vcb);

        Vpb->SerialNumber = Vcb->SerialNumber;
        Vpb->DeviceObject = VolumeDevice;
        Vcb->VcbState |= VCB_STATE_MOUNTED;
        Vcb->VerifyThread = NULL;

        (VOID)ObReferenceObjectByPointer(TargetDevice, 0, NULL, KernelMode);

        (VOID)ExAcquireResourceExclusiveLite(&HpfsData.Resource, TRUE);
        InsertTailList(&HpfsData.VcbList, &Vcb->VcbLinks);
        HpfsRelease(&HpfsData.Resource);

        HPFS_DBG((HPFS_PFX "Mounted HPFS%s %08lX: %lu sectors, code page %u%s%s\n",
                  Vcb->Vol.Hpfs386 ? "386" : "", Vcb->SerialNumber, Vcb->Vol.TotalSectors,
                  Vcb->Vol.CodePage,
                  (Vcb->VcbState & VCB_STATE_KEEP_DIRTY) ? ", dirty" : "",
                  (Vcb->VcbState & VCB_STATE_READ_ONLY) ? ", read-only" : ""));

    } __finally {

        if (Sector != NULL) {
            ExFreePool(Sector);
        }

        if (!NT_SUCCESS(Status)) {

            if (Vcb != NULL) {

                Vpb->DeviceObject = NULL;

                if (ClearedVerify) {
                    RealDevice->Flags |= DO_VERIFY_VOLUME;
                }

                while (!IsListEmpty(&Vcb->FcbList)) {
                    HpfsDeleteFcb(CONTAINING_RECORD(Vcb->FcbList.Flink, HPFS_FCB, FcbLinks));
                }
                if (Vcb->VolumeFcb != NULL) {
                    HpfsDeleteFcb(Vcb->VolumeFcb);
                }

                HpfsMetaPurge(Vcb);
                HpfsUnmountFree(&Vcb->Vol);
                FsRtlNotifyUninitializeSync(&Vcb->NotifySync);
                if (Resources) {
                    ExDeleteResourceLite(&Vcb->EngineResource);
                    ExDeleteResourceLite(&Vcb->Resource);
                }
            }

            if (VolumeDevice != NULL) {
                IoDeleteDevice(VolumeDevice);
            }
        }
    }

    return Status;
}

/* ------------------------------------------------------------------ */
/* Verify                                                              */
/* ------------------------------------------------------------------ */

static NTSTATUS
HpfsVerifyVolume (
    PHPFS_IRP_CONTEXT Ctx
    )
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PVPB Vpb = IrpSp->Parameters.VerifyVolume.Vpb;
    PDEVICE_OBJECT VolumeDevice = IrpSp->Parameters.VerifyVolume.DeviceObject;
    PHPFS_VCB Vcb;
    PHPFS_BOOT_SECTOR Boot = NULL;
    BOOLEAN Delete = FALSE;
    NTSTATUS Status = STATUS_SUCCESS;

    if (VolumeDevice == NULL || VolumeDevice->DriverObject != HpfsData.DriverObject ||
        VolumeDevice == HpfsData.FileSystemDeviceObject) {

        return STATUS_WRONG_VOLUME;
    }

    Vcb = (PHPFS_VCB)VolumeDevice->DeviceExtension;

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        if (Vcb->VcbState & VCB_STATE_DISMOUNTED) {
            Status = STATUS_WRONG_VOLUME;
            __leave;
        }

        if (!(Vpb->RealDevice->Flags & DO_VERIFY_VOLUME)) {
            __leave;
        }

        Vcb->VerifyThread = KeGetCurrentThread();

        Boot = (PHPFS_BOOT_SECTOR)ExAllocatePoolWithTag(NonPagedPool, HPFS_SECTOR_SIZE, HPFS_TAG_BUFFER);
        if (Boot == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        Status = HpfsReadSectors(Vcb->TargetDeviceObject, 0, HPFS_SECTOR_SIZE, Boot, TRUE);

        if (NT_SUCCESS(Status) && HpfsIsBootSector(Boot) &&
            Boot->SerialNumber == Vcb->SerialNumber) {

            /* Same volume; its structures may have been changed while away */
            Status = HpfsLoadVolume(Vcb);
            if (NT_SUCCESS(Status)) {
                HpfsLoadLabel(Vcb);
            } else {
                Delete = HpfsDismountVcb(Vcb);
                Status = STATUS_WRONG_VOLUME;
            }

        } else {

            HPFS_DBG((HPFS_PFX "Verify: volume %08lX is gone\n", Vcb->SerialNumber));

            Delete = HpfsDismountVcb(Vcb);
            Status = STATUS_WRONG_VOLUME;
        }

        Vpb->RealDevice->Flags &= ~DO_VERIFY_VOLUME;

    } __finally {

        if (Boot != NULL) {
            ExFreePool(Boot);
        }

        Vcb->VerifyThread = NULL;
        HpfsRelease(&Vcb->Resource);
    }

    if (Delete) {
        HpfsDeleteVcb(Vcb);
        Ctx->Vcb = NULL;
    }

    return Status;
}

/* ------------------------------------------------------------------ */
/* User requests                                                       */
/* ------------------------------------------------------------------ */

static NTSTATUS
HpfsGetVolumeOpen (
    PHPFS_IRP_CONTEXT Ctx,
    PHPFS_CCB *Ccb
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PHPFS_FCB Fcb;

    if (FileObject == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    Fcb = (PHPFS_FCB)FileObject->FsContext;
    *Ccb = (PHPFS_CCB)FileObject->FsContext2;

    if (Fcb == NULL || *Ccb == NULL || !HpfsIsVfcb(Fcb)) {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

static VOID
HpfsNotifyVolume (
    PFILE_OBJECT FileObject,
    ULONG Event
    )
{
#ifndef HPFS_NT4
    (VOID)FsRtlNotifyVolumeEvent(FileObject, Event);
#else
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(Event);
#endif
}

#ifndef FSRTL_VOLUME_DISMOUNT
#define FSRTL_VOLUME_DISMOUNT       1
#endif
#ifndef FSRTL_VOLUME_LOCK
#define FSRTL_VOLUME_LOCK           3
#endif
#ifndef FSRTL_VOLUME_LOCK_FAILED
#define FSRTL_VOLUME_LOCK_FAILED    4
#endif
#ifndef FSRTL_VOLUME_UNLOCK
#define FSRTL_VOLUME_UNLOCK         5
#endif

static NTSTATUS
HpfsLockVolume (
    PHPFS_IRP_CONTEXT Ctx
    )
{
    PHPFS_VCB Vcb = Ctx->Vcb;
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PHPFS_CCB Ccb;
    KIRQL Irql;
    NTSTATUS Status;

    Status = HpfsGetVolumeOpen(Ctx, &Ccb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    HpfsNotifyVolume(FileObject, FSRTL_VOLUME_LOCK);

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        if (Vcb->VcbState & (VCB_STATE_LOCKED | VCB_STATE_PNP_LOCKED)) {
            Status = STATUS_ACCESS_DENIED;
            __leave;
        }

        /* Whoever locks is about to work on the disk itself */
        if (!(Vcb->VcbState & VCB_STATE_DISMOUNTED)) {
            (VOID)HpfsFlushVolume(Vcb, TRUE);
            HpfsPurgeCachedFiles(Vcb);
        }

        /* Only the caller's handle may be open */
        if (Vcb->UncleanCount != 1) {
            Status = STATUS_ACCESS_DENIED;
            __leave;
        }

        Vcb->VcbState |= VCB_STATE_LOCKED;
        Vcb->LockFileObject = FileObject;

        IoAcquireVpbSpinLock(&Irql);
        Vcb->Vpb->Flags |= VPB_LOCKED;
        IoReleaseVpbSpinLock(Irql);

    } __finally {

        HpfsRelease(&Vcb->Resource);
    }

    if (!NT_SUCCESS(Status)) {
        HpfsNotifyVolume(FileObject, FSRTL_VOLUME_LOCK_FAILED);
    }

    return Status;
}

static NTSTATUS
HpfsUnlockVolume (
    PHPFS_IRP_CONTEXT Ctx
    )
{
    PHPFS_VCB Vcb = Ctx->Vcb;
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PHPFS_CCB Ccb;
    NTSTATUS Status;

    Status = HpfsGetVolumeOpen(Ctx, &Ccb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        if (Vcb->LockFileObject != FileObject) {

            Status = STATUS_NOT_LOCKED;

        } else {

            HpfsUnlockVcb(Vcb);

            /* Metadata may have been rewritten under us */
            if (Vcb->VcbState & VCB_STATE_DASD_WRITTEN) {
                (VOID)HpfsDismountVcb(Vcb);
            }
        }

    } __finally {

        HpfsRelease(&Vcb->Resource);
    }

    if (NT_SUCCESS(Status)) {
        HpfsNotifyVolume(FileObject, FSRTL_VOLUME_UNLOCK);
    }

    return Status;
}

static NTSTATUS
HpfsDismountVolume (
    PHPFS_IRP_CONTEXT Ctx
    )
{
    PHPFS_VCB Vcb = Ctx->Vcb;
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PHPFS_CCB Ccb;
    NTSTATUS Status;

    Status = HpfsGetVolumeOpen(Ctx, &Ccb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    HpfsNotifyVolume(FileObject, FSRTL_VOLUME_DISMOUNT);

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        if (!(Vcb->VcbState & VCB_STATE_DISMOUNTED)) {
            (VOID)HpfsFlushVolume(Vcb, TRUE);
        }

        /* The caller's handle stays open, so the VCB cannot go away here */
        Ccb->Flags |= CCB_FLAG_DISMOUNTED_VOLUME;
        (VOID)HpfsDismountVcb(Vcb);

    } __finally {

        HpfsRelease(&Vcb->Resource);
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
HpfsIsVolumeDirty (
    PHPFS_IRP_CONTEXT Ctx
    )
{
    PHPFS_VCB Vcb = Ctx->Vcb;
    PIRP Irp = Ctx->Irp;
    PULONG Result = (PULONG)Irp->AssociatedIrp.SystemBuffer;

    if (HpfsXSp(Ctx->IrpSp)->Parameters.FileSystemControl.OutputBufferLength < sizeof(ULONG)) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Dirty for chkdsk, not merely in use */
    *Result = (Vcb->VcbState & VCB_STATE_KEEP_DIRTY) ? VOLUME_IS_DIRTY : 0;
    Irp->IoStatus.Information = sizeof(ULONG);

    return STATUS_SUCCESS;
}

static NTSTATUS
HpfsMarkVolumeDirtyRequest (
    PHPFS_IRP_CONTEXT Ctx
    )
{
    PHPFS_VCB Vcb = Ctx->Vcb;
    NTSTATUS Status;

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Status = HpfsVerifyWritable(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        Vcb->VcbState |= VCB_STATE_KEEP_DIRTY;
        HpfsMarkVolumeDirty(Vcb);

    } __finally {

        HpfsRelease(&Vcb->Resource);
    }

    return Status;
}

/* HPFS386 DASD limits and the volume's particulars (hpfsctl.h) */
static NTSTATUS
HpfsPrivateRequest (
    PHPFS_IRP_CONTEXT Ctx,
    ULONG Code
    )
{
    PHPFS_VCB Vcb = Ctx->Vcb;
    PIRP Irp = Ctx->Irp;
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PHPFS_FCB Fcb = (FileObject != NULL) ? (PHPFS_FCB)FileObject->FsContext : NULL;
    ULONG InLength = HpfsXSp(Ctx->IrpSp)->Parameters.FileSystemControl.InputBufferLength;
    ULONG OutLength = HpfsXSp(Ctx->IrpSp)->Parameters.FileSystemControl.OutputBufferLength;
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    HPFS_DASD Dasd;
    NTSTATUS Status;

    if (Fcb == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Status = HpfsVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        if (Code == HPFS_FSCTL_QUERY_VOLUME) {
            PHPFS_VOLUME_INFO Info = (PHPFS_VOLUME_INFO)Buffer;
            ULONG Free = 0;
            if (OutLength < sizeof(HPFS_VOLUME_INFO)) {
                Status = STATUS_BUFFER_TOO_SMALL;
                __leave;
            }
            HpfsEngineEnter(Vcb);
            Status = HpfsEngineLeave(Vcb, HpfsCountFree(&Vcb->Vol, &Free));
            if (!NT_SUCCESS(Status)) {
                __leave;
            }
            RtlZeroMemory(Info, sizeof(HPFS_VOLUME_INFO));
            Info->TotalSectors = Vcb->Vol.TotalSectors;
            Info->FreeSectors = Free;
            Info->Version = Vcb->Vol.Version;
            Info->FuncVersion = Vcb->Vol.FuncVersion;
            Info->SpareFlags = Vcb->Vol.SpareFlags;
            Info->SpareFlags2 = Vcb->Vol.SpareFlags2;
            Info->Hpfs386 = Vcb->Vol.Hpfs386;
            Info->DasdActive = Vcb->Vol.DasdActive;
            Info->CodePage = Vcb->Vol.CodePage;
            Info->HotfixCount = Vcb->Vol.HotfixCount;
            Info->SpareDnodes = Vcb->Vol.SpareDnodes;
            Info->SpareDnodesFree = Vcb->Vol.SpareDnodesFree;
            Info->DirBandDnodes = Vcb->Vol.DirBandDnodes;
            Info->ReadOnly = (Vcb->VcbState & VCB_STATE_READ_ONLY) ? 1 : 0;
            Irp->IoStatus.Information = sizeof(HPFS_VOLUME_INFO);
            __leave;
        }

        if (!HpfsIsDcb(Fcb)) {
            Status = STATUS_NOT_A_DIRECTORY;
            __leave;
        }

        if (Code == HPFS_FSCTL_SET_DASD) {
            PHPFS_DASD_INFO In = (PHPFS_DASD_INFO)Buffer;
            if (InLength < sizeof(HPFS_DASD_INFO) || In->Threshold > 100 || In->Delta > 100) {
                Status = STATUS_INVALID_PARAMETER;
                __leave;
            }
            Status = HpfsVerifyWritable(Ctx, Vcb);
            if (!NT_SUCCESS(Status)) {
                __leave;
            }
            /* Sizes and allocation of open files must be on the disk to be counted */
            (VOID)HpfsFlushVolume(Vcb, FALSE);
            HpfsEngineEnter(Vcb);
            Status = HpfsEngineLeave(Vcb, HpfsDasdSet(&Vcb->Vol, Fcb->Fnode, In->Limit,
                                                      In->Threshold, In->Delta));
            if (!NT_SUCCESS(Status)) {
                __leave;
            }
        }

        if (OutLength < sizeof(HPFS_DASD_INFO)) {
            Status = (Code == HPFS_FSCTL_SET_DASD) ? STATUS_SUCCESS : STATUS_BUFFER_TOO_SMALL;
            __leave;
        }
        HpfsEngineEnter(Vcb);
        Status = HpfsEngineLeave(Vcb, HpfsDasdQuery(&Vcb->Vol, Fcb->Fnode, &Dasd));
        if (NT_SUCCESS(Status)) {
            PHPFS_DASD_INFO Out = (PHPFS_DASD_INFO)Buffer;
            Out->Limit = Dasd.Limit;
            Out->Usage = Dasd.Usage;
            Out->Threshold = Dasd.Threshold;
            Out->Delta = Dasd.Delta;
            Out->Active = Dasd.Active;
            Irp->IoStatus.Information = sizeof(HPFS_DASD_INFO);
        }

    } __finally {

        HpfsRelease(&Vcb->Resource);
    }

    return Status;
}

NTSTATUS
HpfsUserFsRequest (
    PHPFS_IRP_CONTEXT Ctx
    )
{
    ULONG Code = HpfsXSp(Ctx->IrpSp)->Parameters.FileSystemControl.FsControlCode;

    switch (Code) {

    case FSCTL_LOCK_VOLUME:
        return HpfsLockVolume(Ctx);

    case FSCTL_UNLOCK_VOLUME:
        return HpfsUnlockVolume(Ctx);

    case FSCTL_DISMOUNT_VOLUME:
        return HpfsDismountVolume(Ctx);

    case FSCTL_IS_VOLUME_MOUNTED:
        return HpfsVerifyVcb(Ctx, Ctx->Vcb);

    case FSCTL_IS_PATHNAME_VALID:
        return STATUS_SUCCESS;

    case FSCTL_IS_VOLUME_DIRTY:
        return HpfsIsVolumeDirty(Ctx);

    case FSCTL_MARK_VOLUME_DIRTY:
        return HpfsMarkVolumeDirtyRequest(Ctx);

    case HPFS_FSCTL_QUERY_DASD:
    case HPFS_FSCTL_SET_DASD:
    case HPFS_FSCTL_QUERY_VOLUME:
        return HpfsPrivateRequest(Ctx, Code);

    case FSCTL_REQUEST_OPLOCK_LEVEL_1:
    case FSCTL_REQUEST_OPLOCK_LEVEL_2:
    case FSCTL_REQUEST_BATCH_OPLOCK:
    case FSCTL_REQUEST_FILTER_OPLOCK:
        return STATUS_OPLOCK_NOT_GRANTED;

    case FSCTL_OPLOCK_BREAK_ACKNOWLEDGE:
    case FSCTL_OPBATCH_ACK_CLOSE_PENDING:
    case FSCTL_OPLOCK_BREAK_NOTIFY:
    case FSCTL_OPLOCK_BREAK_ACK_NO_2:
        return STATUS_INVALID_OPLOCK_PROTOCOL;
    }

    return STATUS_INVALID_DEVICE_REQUEST;
}

NTSTATUS
HpfsCommonFileSystemControl (
    PHPFS_IRP_CONTEXT Ctx
    )
{
    switch (Ctx->IrpSp->MinorFunction) {

    case IRP_MN_MOUNT_VOLUME:
        if (Ctx->Vcb != NULL) {
            return STATUS_INVALID_DEVICE_REQUEST;
        }
        return HpfsMountVolume(Ctx);

    case IRP_MN_VERIFY_VOLUME:
        return HpfsVerifyVolume(Ctx);

    case IRP_MN_USER_FS_REQUEST:
#ifdef IRP_MN_KERNEL_CALL
    case IRP_MN_KERNEL_CALL:
#endif
        if (Ctx->Vcb == NULL) {
            return STATUS_INVALID_DEVICE_REQUEST;
        }
        return HpfsUserFsRequest(Ctx);
    }

    return STATUS_INVALID_DEVICE_REQUEST;
}

/* ------------------------------------------------------------------ */
/* Plug and Play (Windows 2000)                                        */
/* ------------------------------------------------------------------ */

#ifndef HPFS_NT4

static NTSTATUS
NTAPI
HpfsPnpCompletion (
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

/* Sends the IRP down and waits; the caller completes it */
static NTSTATUS
HpfsForwardAndWait (
    PHPFS_IRP_CONTEXT Ctx,
    PHPFS_VCB Vcb
    )
{
    PIRP Irp = Ctx->Irp;
    KEVENT Event;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    HpfsCopyStackToNext(Irp);
    IoSetCompletionRoutine(Irp, HpfsPnpCompletion, &Event, TRUE, TRUE, TRUE);

    if (IoCallDriver(Vcb->TargetDeviceObject, Irp) == STATUS_PENDING) {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    }

    return Irp->IoStatus.Status;
}

NTSTATUS
HpfsCommonPnp (
    PHPFS_IRP_CONTEXT Ctx
    )
{
    PHPFS_VCB Vcb = Ctx->Vcb;
    PIRP Irp = Ctx->Irp;
    BOOLEAN Delete = FALSE;
    NTSTATUS Status;

    switch (Ctx->IrpSp->MinorFunction) {

    case IRP_MN_QUERY_REMOVE_DEVICE:

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

        if (!(Vcb->VcbState & VCB_STATE_DISMOUNTED)) {
            (VOID)HpfsFlushVolume(Vcb, TRUE);
            HpfsPurgeCachedFiles(Vcb);
        }

        if (Vcb->UncleanCount != 0 || (Vcb->VcbState & VCB_STATE_LOCKED)) {
            HpfsRelease(&Vcb->Resource);
            return STATUS_ACCESS_DENIED;
        }

        Vcb->VcbState |= VCB_STATE_PNP_LOCKED;
        HpfsRelease(&Vcb->Resource);

        Status = HpfsForwardAndWait(Ctx, Vcb);

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

        if (NT_SUCCESS(Status)) {
            Delete = HpfsDismountVcb(Vcb);
        } else {
            Vcb->VcbState &= ~VCB_STATE_PNP_LOCKED;
        }

        HpfsRelease(&Vcb->Resource);
        break;

    case IRP_MN_REMOVE_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:

        Status = HpfsForwardAndWait(Ctx, Vcb);

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
        Vcb->VcbState &= ~VCB_STATE_PNP_LOCKED;
        Vcb->VcbState |= VCB_STATE_REMOVED;
        Delete = HpfsDismountVcb(Vcb);
        HpfsRelease(&Vcb->Resource);
        break;

    case IRP_MN_CANCEL_REMOVE_DEVICE:

        Status = HpfsForwardAndWait(Ctx, Vcb);

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
        Vcb->VcbState &= ~VCB_STATE_PNP_LOCKED;
        HpfsRelease(&Vcb->Resource);
        break;

    default:

        if (Vcb->VcbState & VCB_STATE_REMOVED) {
            return STATUS_NO_SUCH_DEVICE;
        }

        HpfsSkipStack(Irp);
        Ctx->Flags |= HPFS_CTX_NO_COMPLETE;
        return IoCallDriver(Vcb->TargetDeviceObject, Irp);
    }

    if (Delete) {
        HpfsDeleteVcb(Vcb);
        Ctx->Vcb = NULL;
    }

    return Status;
}

#else

NTSTATUS
HpfsCommonPnp (
    PHPFS_IRP_CONTEXT Ctx
    )
{
    UNREFERENCED_PARAMETER(Ctx);
    return STATUS_INVALID_DEVICE_REQUEST;
}

#endif
