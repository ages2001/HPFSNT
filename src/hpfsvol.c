/*
 * HPFSNT - IRP_MJ_QUERY_VOLUME_INFORMATION and IRP_MJ_SET_VOLUME_INFORMATION
 */

#include "hpfs.h"

static NTSTATUS
HpfsCopyString (
    PWSTR Target,
    ULONG Room,
    const WCHAR *Source,
    ULONG Bytes,
    PULONG Copied
    )
{
    *Copied = (Bytes < Room) ? Bytes : Room;
    RtlCopyMemory(Target, Source, *Copied);

    return (*Copied < Bytes) ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}

static NTSTATUS
HpfsVolumeFree (
    PHPFS_VCB Vcb,
    PULONG Free
    )
{
    NTSTATUS Status;

    HpfsEngineEnter(Vcb);
    Status = HpfsEngineLeave(Vcb, HpfsCountFree(&Vcb->Vol, Free));
    return Status;
}

NTSTATUS
HpfsCommonQueryVolumeInformation (
    PHPFS_IRP_CONTEXT Ctx
    )
{
    PIRP Irp = Ctx->Irp;
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PHPFS_VCB Vcb = Ctx->Vcb;
    ULONG Class = IrpSp->Parameters.QueryVolume.FsInformationClass;
    ULONG Length = IrpSp->Parameters.QueryVolume.Length;
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    PFILE_FS_VOLUME_INFORMATION Volume;
    PFILE_FS_SIZE_INFORMATION Size;
    PFILE_FS_DEVICE_INFORMATION Device;
    PFILE_FS_ATTRIBUTE_INFORMATION Attribute;
    PHPFS_FILE_FS_FULL_SIZE_INFORMATION FullSize;
    ULONG Base;
    ULONG Copied;
    ULONG Free = 0;
    ULONG Used = 0;
    NTSTATUS Status = STATUS_SUCCESS;

    (VOID)ExAcquireResourceSharedLite(&Vcb->Resource, TRUE);

    __try {

        Status = HpfsVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        switch (Class) {

        case FileFsVolumeInformation:
            Base = FIELD_OFFSET(FILE_FS_VOLUME_INFORMATION, VolumeLabel);
            if (Length < Base) {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }

            Volume = (PFILE_FS_VOLUME_INFORMATION)Buffer;
            RtlZeroMemory(Volume, Base);
            Volume->VolumeSerialNumber = Vcb->SerialNumber;
            Volume->VolumeLabelLength = Vcb->LabelLength * sizeof(WCHAR);
            Volume->SupportsObjects = FALSE;

            Status = HpfsCopyString(Volume->VolumeLabel, Length - Base, Vcb->Label,
                                    Vcb->LabelLength * sizeof(WCHAR), &Copied);
            Used = Base + Copied;
            break;

        case FileFsSizeInformation:
            if (Length < sizeof(FILE_FS_SIZE_INFORMATION)) {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }
            Status = HpfsVolumeFree(Vcb, &Free);
            if (!NT_SUCCESS(Status)) {
                break;
            }
            Size = (PFILE_FS_SIZE_INFORMATION)Buffer;
            Size->TotalAllocationUnits.QuadPart = Vcb->Vol.TotalSectors;
            Size->AvailableAllocationUnits.QuadPart = Free;
            Size->SectorsPerAllocationUnit = 1;
            Size->BytesPerSector = HPFS_SECTOR_SIZE;
            Used = sizeof(FILE_FS_SIZE_INFORMATION);
            break;

        case HPFS_CLASS_FS_FULL_SIZE:
            if (Length < sizeof(HPFS_FILE_FS_FULL_SIZE_INFORMATION)) {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }
            Status = HpfsVolumeFree(Vcb, &Free);
            if (!NT_SUCCESS(Status)) {
                break;
            }
            FullSize = (PHPFS_FILE_FS_FULL_SIZE_INFORMATION)Buffer;
            FullSize->TotalAllocationUnits.QuadPart = Vcb->Vol.TotalSectors;
            FullSize->CallerAvailableAllocationUnits.QuadPart = Free;
            FullSize->ActualAvailableAllocationUnits.QuadPart = Free;
            FullSize->SectorsPerAllocationUnit = 1;
            FullSize->BytesPerSector = HPFS_SECTOR_SIZE;
            Used = sizeof(HPFS_FILE_FS_FULL_SIZE_INFORMATION);
            break;

        case FileFsDeviceInformation:
            if (Length < sizeof(FILE_FS_DEVICE_INFORMATION)) {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }
            Device = (PFILE_FS_DEVICE_INFORMATION)Buffer;
            Device->DeviceType = FILE_DEVICE_DISK;
            Device->Characteristics = Vcb->Vpb->RealDevice->Characteristics;
            Used = sizeof(FILE_FS_DEVICE_INFORMATION);
            break;

        case FileFsAttributeInformation:
            Base = FIELD_OFFSET(FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName);
            if (Length < Base) {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }

            Attribute = (PFILE_FS_ATTRIBUTE_INFORMATION)Buffer;
            Attribute->FileSystemAttributes = FILE_CASE_PRESERVED_NAMES;
            if (Vcb->VcbState & VCB_STATE_READ_ONLY) {
                Attribute->FileSystemAttributes |= FILE_READ_ONLY_VOLUME;
            }
            Attribute->MaximumComponentNameLength = HPFS_MAX_NAME;
            Attribute->FileSystemNameLength = sizeof(HPFS_FS_NAME) - sizeof(WCHAR);

            Status = HpfsCopyString(Attribute->FileSystemName, Length - Base, HPFS_FS_NAME,
                                    sizeof(HPFS_FS_NAME) - sizeof(WCHAR), &Copied);
            Used = Base + Copied;
            break;

        default:
            Status = STATUS_INVALID_PARAMETER;
            break;
        }

        if (NT_SUCCESS(Status) || Status == STATUS_BUFFER_OVERFLOW) {
            Irp->IoStatus.Information = Used;
        }

    } __finally {

        HpfsRelease(&Vcb->Resource);
    }

    return Status;
}

/* Some NT4 headers lack FILE_FS_LABEL_INFORMATION */
typedef struct _HPFS_FS_LABEL {
    ULONG       VolumeLabelLength;
    WCHAR       VolumeLabel[1];
} HPFS_FS_LABEL, *PHPFS_FS_LABEL;

NTSTATUS
HpfsCommonSetVolumeInformation (
    PHPFS_IRP_CONTEXT Ctx
    )
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PHPFS_VCB Vcb = Ctx->Vcb;
    PHPFS_FS_LABEL Label = (PHPFS_FS_LABEL)Ctx->Irp->AssociatedIrp.SystemBuffer;
    /* NT4 ntddk.h has no SetVolume member; QueryVolume has the same layout */
    ULONG Length = IrpSp->Parameters.QueryVolume.Length;
    UCHAR Oem[HPFS_MAX_LABEL + 1];
    ULONG OemLength = 0;
    ULONG Count;
    ULONG i;
    NTSTATUS Status;

    if (IrpSp->Parameters.QueryVolume.FsInformationClass != FileFsLabelInformation) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Length < FIELD_OFFSET(HPFS_FS_LABEL, VolumeLabel) ||
        Label->VolumeLabelLength > Length - FIELD_OFFSET(HPFS_FS_LABEL, VolumeLabel) ||
        (Label->VolumeLabelLength & 1) != 0 ||
        Label->VolumeLabelLength > HPFS_MAX_LABEL * sizeof(WCHAR)) {

        return STATUS_INVALID_VOLUME_LABEL;
    }

    Count = Label->VolumeLabelLength / sizeof(WCHAR);

    for (i = 0; i < Count; i++) {
        WCHAR C = Label->VolumeLabel[i];
        if (C < 0x20 || C == L'*' || C == L'?' || C == L'/' || C == L'\\' || C == L'|' ||
            C == L'.' || C == L',' || C == L';' || C == L':' || C == L'+' || C == L'=' ||
            C == L'[' || C == L']' || C == L'<' || C == L'>' || C == L'"') {
            return STATUS_INVALID_VOLUME_LABEL;
        }
    }

    if (Count != 0) {
        Status = RtlUpcaseUnicodeToOemN((PCHAR)Oem, HPFS_MAX_LABEL, &OemLength,
                                        Label->VolumeLabel, Count * sizeof(WCHAR));
        if (!NT_SUCCESS(Status) || OemLength > HPFS_MAX_LABEL) {
            return STATUS_INVALID_VOLUME_LABEL;
        }
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Status = HpfsVerifyWritable(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        HpfsEngineEnter(Vcb);
        Status = HpfsEngineLeave(Vcb, HpfsSetLabel(&Vcb->Vol, Oem, OemLength));
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        HpfsLoadLabel(Vcb);

    } __finally {

        HpfsRelease(&Vcb->Resource);
    }

    return Status;
}
