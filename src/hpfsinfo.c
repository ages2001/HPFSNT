/*
 * HPFSNT - IRP_MJ_QUERY_INFORMATION
 */

#include "hpfs.h"

/* HPFS keeps the DOS attribute bits, which have the NT values */
ULONG
HpfsNtAttributes (
    UCHAR Attributes
    )
{
    ULONG Nt = Attributes & (HPFS_ATTR_READONLY | HPFS_ATTR_HIDDEN | HPFS_ATTR_SYSTEM |
                             HPFS_ATTR_DIRECTORY | HPFS_ATTR_ARCHIVE);

    if (Nt == 0) {
        Nt = FILE_ATTRIBUTE_NORMAL;
    }

    return Nt;
}

VOID
HpfsFillBasicInfo (
    PHPFS_FCB Fcb,
    PFILE_BASIC_INFORMATION Info
    )
{
    Info->CreationTime = Fcb->CreationTime;
    Info->LastAccessTime = Fcb->LastAccessTime;
    Info->LastWriteTime = Fcb->LastWriteTime;
    Info->ChangeTime = Fcb->LastWriteTime;
    Info->FileAttributes = HpfsNtAttributes(Fcb->Attributes);
}

VOID
HpfsFillStandardInfo (
    PHPFS_FCB Fcb,
    PFILE_STANDARD_INFORMATION Info
    )
{
    Info->NumberOfLinks = 1;
    Info->DeletePending = (BOOLEAN)((Fcb->FcbState & FCB_STATE_DELETE_PENDING) != 0);

    if (HpfsIsDcb(Fcb)) {
        Info->AllocationSize.QuadPart = 0;
        Info->EndOfFile.QuadPart = 0;
        Info->Directory = TRUE;
    } else {
        Info->AllocationSize = Fcb->Header.AllocationSize;
        Info->EndOfFile = Fcb->Header.FileSize;
        Info->Directory = FALSE;
    }
}

VOID
HpfsFillNetworkOpenInfo (
    PHPFS_FCB Fcb,
    PFILE_NETWORK_OPEN_INFORMATION Info
    )
{
    FILE_STANDARD_INFORMATION Standard;

    HpfsFillStandardInfo(Fcb, &Standard);

    Info->CreationTime = Fcb->CreationTime;
    Info->LastAccessTime = Fcb->LastAccessTime;
    Info->LastWriteTime = Fcb->LastWriteTime;
    Info->ChangeTime = Fcb->LastWriteTime;
    Info->AllocationSize = Standard.AllocationSize;
    Info->EndOfFile = Standard.EndOfFile;
    Info->FileAttributes = HpfsNtAttributes(Fcb->Attributes);
}

/*
 * Copies as much of "\dir\file" as fits in Room bytes and returns the
 * full length in bytes.
 */
static NTSTATUS
HpfsCopyPath (
    PHPFS_FCB Fcb,
    PWSTR Target,
    ULONG Room,
    PULONG FullLength,
    PULONG Copied
    )
{
    PHPFS_FCB Walk;
    ULONG Length = 0;
    PWSTR Path;
    PWSTR Write;

    if (Fcb->ParentDcb == NULL) {
        Length = sizeof(WCHAR);
    } else {
        for (Walk = Fcb; Walk->ParentDcb != NULL; Walk = Walk->ParentDcb) {
            Length += sizeof(WCHAR) + Walk->Name.Length;
        }
    }

    Path = (PWSTR)ExAllocatePoolWithTag(PagedPool, Length, HPFS_TAG_NAME);
    if (Path == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (Fcb->ParentDcb == NULL) {

        Path[0] = L'\\';

    } else {

        Write = (PWSTR)((PUCHAR)Path + Length);

        for (Walk = Fcb; Walk->ParentDcb != NULL; Walk = Walk->ParentDcb) {
            Write = (PWSTR)((PUCHAR)Write - Walk->Name.Length);
            RtlCopyMemory(Write, Walk->Name.Buffer, Walk->Name.Length);
            Write--;
            *Write = L'\\';
        }
    }

    *FullLength = Length;
    *Copied = (Length < Room) ? Length : Room;

    RtlCopyMemory(Target, Path, *Copied);
    ExFreePool(Path);

    return STATUS_SUCCESS;
}

static NTSTATUS
HpfsQueryNameInfo (
    PHPFS_FCB Fcb,
    PFILE_NAME_INFORMATION Info,
    ULONG Length,
    PULONG Used
    )
{
    ULONG Base = FIELD_OFFSET(FILE_NAME_INFORMATION, FileName);
    ULONG FullLength;
    ULONG Copied;
    NTSTATUS Status;

    if (Length < Base) {
        return STATUS_INFO_LENGTH_MISMATCH;
    }

    Status = HpfsCopyPath(Fcb, Info->FileName, Length - Base, &FullLength, &Copied);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Info->FileNameLength = FullLength;
    *Used = Base + Copied;

    return (Copied < FullLength) ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}

/* Handles opened on the volume itself */
static NTSTATUS
HpfsQueryVolumeFileInfo (
    PHPFS_IRP_CONTEXT Ctx,
    PHPFS_FCB Vfcb,
    FILE_INFORMATION_CLASS Class,
    PVOID Buffer,
    ULONG Length
    )
{
    PHPFS_VCB Vcb = Vfcb->Vcb;
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PFILE_STANDARD_INFORMATION Standard;
    PFILE_BASIC_INFORMATION Basic;

    switch (Class) {

    case FilePositionInformation:
        ((PFILE_POSITION_INFORMATION)Buffer)->CurrentByteOffset = FileObject->CurrentByteOffset;
        Ctx->Irp->IoStatus.Information = sizeof(FILE_POSITION_INFORMATION);
        return STATUS_SUCCESS;

    case FileStandardInformation:
        Standard = (PFILE_STANDARD_INFORMATION)Buffer;
        RtlZeroMemory(Standard, sizeof(FILE_STANDARD_INFORMATION));
        Standard->AllocationSize.QuadPart = Vcb->PartitionBytes;
        Standard->EndOfFile.QuadPart = Vcb->PartitionBytes;
        Standard->NumberOfLinks = 1;
        Ctx->Irp->IoStatus.Information = sizeof(FILE_STANDARD_INFORMATION);
        return STATUS_SUCCESS;

    case FileBasicInformation:
        Basic = (PFILE_BASIC_INFORMATION)Buffer;
        RtlZeroMemory(Basic, sizeof(FILE_BASIC_INFORMATION));
        Basic->FileAttributes = FILE_ATTRIBUTE_NORMAL;
        Ctx->Irp->IoStatus.Information = sizeof(FILE_BASIC_INFORMATION);
        return STATUS_SUCCESS;

    default:
        break;
    }

    UNREFERENCED_PARAMETER(Length);
    return STATUS_INVALID_PARAMETER;
}

NTSTATUS
HpfsCommonQueryInformation (
    PHPFS_IRP_CONTEXT Ctx
    )
{
    PIRP Irp = Ctx->Irp;
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PHPFS_FCB Fcb = (PHPFS_FCB)FileObject->FsContext;
    PHPFS_VCB Vcb = Ctx->Vcb;
    FILE_INFORMATION_CLASS Class = IrpSp->Parameters.QueryFile.FileInformationClass;
    ULONG Length = IrpSp->Parameters.QueryFile.Length;
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    PFILE_ALL_INFORMATION All;
    PHPFS_FILE_ATTRIBUTE_TAG_INFORMATION Tag;
    ULONG Used = 0;
    ULONG Base;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Fcb == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (HpfsIsVfcb(Fcb)) {
        return HpfsQueryVolumeFileInfo(Ctx, Fcb, Class, Buffer, Length);
    }

    (VOID)ExAcquireResourceSharedLite(&Vcb->Resource, TRUE);
    (VOID)ExAcquireResourceSharedLite(&Fcb->Resource, TRUE);

    __try {

        Status = HpfsVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        switch ((ULONG)Class) {

        case FileBasicInformation:
            HpfsFillBasicInfo(Fcb, (PFILE_BASIC_INFORMATION)Buffer);
            Used = sizeof(FILE_BASIC_INFORMATION);
            break;

        case FileStandardInformation:
            HpfsFillStandardInfo(Fcb, (PFILE_STANDARD_INFORMATION)Buffer);
            Used = sizeof(FILE_STANDARD_INFORMATION);
            break;

        case FileInternalInformation:
            ((PFILE_INTERNAL_INFORMATION)Buffer)->IndexNumber.QuadPart = Fcb->Fnode;
            Used = sizeof(FILE_INTERNAL_INFORMATION);
            break;

        case FileEaInformation:
            ((PFILE_EA_INFORMATION)Buffer)->EaSize = HpfsNtEaSize(Vcb, Fcb);
            Used = sizeof(FILE_EA_INFORMATION);
            break;

        case FilePositionInformation:
            ((PFILE_POSITION_INFORMATION)Buffer)->CurrentByteOffset = FileObject->CurrentByteOffset;
            Used = sizeof(FILE_POSITION_INFORMATION);
            break;

        case FileNameInformation:
            Status = HpfsQueryNameInfo(Fcb, (PFILE_NAME_INFORMATION)Buffer, Length, &Used);
            break;

        case FileAllInformation:
            All = (PFILE_ALL_INFORMATION)Buffer;
            Base = FIELD_OFFSET(FILE_ALL_INFORMATION, NameInformation);

            if (Length < Base + FIELD_OFFSET(FILE_NAME_INFORMATION, FileName)) {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }

            RtlZeroMemory(All, Base);
            HpfsFillBasicInfo(Fcb, &All->BasicInformation);
            HpfsFillStandardInfo(Fcb, &All->StandardInformation);
            All->InternalInformation.IndexNumber.QuadPart = Fcb->Fnode;
            All->EaInformation.EaSize = HpfsNtEaSize(Vcb, Fcb);
            All->PositionInformation.CurrentByteOffset = FileObject->CurrentByteOffset;
            All->AlignmentInformation.AlignmentRequirement =
                Ctx->DeviceObject->AlignmentRequirement;

            Status = HpfsQueryNameInfo(Fcb, &All->NameInformation, Length - Base, &Used);
            Used += Base;
            break;

        case FileNetworkOpenInformation:
            HpfsFillNetworkOpenInfo(Fcb, (PFILE_NETWORK_OPEN_INFORMATION)Buffer);
            Used = sizeof(FILE_NETWORK_OPEN_INFORMATION);
            break;

        case HPFS_CLASS_ATTRIBUTE_TAG:
            if (Length < sizeof(HPFS_FILE_ATTRIBUTE_TAG_INFORMATION)) {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }
            Tag = (PHPFS_FILE_ATTRIBUTE_TAG_INFORMATION)Buffer;
            Tag->FileAttributes = HpfsNtAttributes(Fcb->Attributes);
            Tag->ReparseTag = 0;
            Used = sizeof(HPFS_FILE_ATTRIBUTE_TAG_INFORMATION);
            break;

        case FileAlternateNameInformation:
            /* HPFS keeps one name per file */
            Status = STATUS_OBJECT_NAME_NOT_FOUND;
            break;

        default:
            Status = STATUS_INVALID_PARAMETER;
            break;
        }

        if (NT_SUCCESS(Status) || Status == STATUS_BUFFER_OVERFLOW) {
            Irp->IoStatus.Information = Used;
        }

    } __finally {

        HpfsRelease(&Fcb->Resource);
        HpfsRelease(&Vcb->Resource);
    }

    return Status;
}
