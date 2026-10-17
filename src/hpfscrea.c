/*
 * HPFSNT - IRP_MJ_CREATE
 *
 * Opens, creates, overwrites and supersedes files and directories (with
 * extended attributes from the create's EA buffer), opens the volume
 * itself, and opens the parent of a rename target
 * (SL_OPEN_TARGET_DIRECTORY). HPFS keeps no NT security: MAXIMUM_ALLOWED
 * is granted as everything the read-only attribute and the medium allow.
 */

#include "hpfs.h"

#ifndef FILE_ALL_ACCESS
#define FILE_ALL_ACCESS (STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | 0x1FF)
#endif
#ifndef FILE_EXISTS
#define FILE_EXISTS                 0x00000004
#endif
#ifndef FILE_DOES_NOT_EXIST
#define FILE_DOES_NOT_EXIST         0x00000005
#endif
#ifndef FILE_NO_EA_KNOWLEDGE
#define FILE_NO_EA_KNOWLEDGE        0x00000200
#endif

#define HPFS_DATA_WRITE_ACCESS  (FILE_WRITE_DATA | FILE_APPEND_DATA)

static BOOLEAN
HpfsNextComponent (
    PUNICODE_STRING Remaining,
    PUNICODE_STRING Component
    )
{
    USHORT Count = Remaining->Length / sizeof(WCHAR);
    USHORT i;

    if (Count == 0) {
        return FALSE;
    }
    for (i = 0; i < Count && Remaining->Buffer[i] != L'\\'; i++) {
        ;
    }
    Component->Buffer = Remaining->Buffer;
    Component->Length = (USHORT)(i * sizeof(WCHAR));
    Component->MaximumLength = Component->Length;
    if (i < Count) {
        i++;
    }
    Remaining->Buffer += i;
    Remaining->Length = (USHORT)(Remaining->Length - i * sizeof(WCHAR));
    Remaining->MaximumLength = Remaining->Length;
    return TRUE;
}

static VOID
HpfsDropUnopened (
    PHPFS_VCB Vcb,
    PHPFS_FCB Fcb
    )
{
    if (Fcb != NULL && Fcb->RefCount == 0 &&
        Fcb != Vcb->RootDcb && Fcb != Vcb->VolumeFcb) {
        Fcb->RefCount = 1;
        HpfsDereferenceFcb(Vcb, Fcb);
    }
}

static ACCESS_MASK
HpfsResolveAccess (
    PIO_STACK_LOCATION IrpSp,
    PHPFS_VCB Vcb,
    PHPFS_FCB Fcb
    )
{
    PIO_SECURITY_CONTEXT SecurityContext = IrpSp->Parameters.Create.SecurityContext;
    PACCESS_STATE AccessState = SecurityContext->AccessState;
    ACCESS_MASK Access = SecurityContext->DesiredAccess;
    ACCESS_MASK Granted;

    if (Access & MAXIMUM_ALLOWED) {

        Granted = FILE_ALL_ACCESS;

        if (Vcb->VcbState & VCB_STATE_READ_ONLY) {
            Granted &= ~HPFS_WRITE_ACCESS;
        } else if (Fcb != NULL && HpfsIsFcb(Fcb) && (Fcb->Attributes & HPFS_ATTR_READONLY)) {
            Granted &= ~HPFS_DATA_WRITE_ACCESS;
        }

        Granted |= Access & ~MAXIMUM_ALLOWED;

        if (AccessState != NULL) {
            AccessState->PreviouslyGrantedAccess |= Granted;
            AccessState->RemainingDesiredAccess &= ~(Granted | MAXIMUM_ALLOWED);
        }

        Access = Granted;
    }

    return Access;
}

static NTSTATUS
HpfsOpenVolume (
    PHPFS_IRP_CONTEXT Ctx,
    PHPFS_VCB Vcb,
    ACCESS_MASK DesiredAccess,
    USHORT ShareAccess,
    ULONG CreateDisposition,
    ULONG CreateOptions
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PHPFS_FCB Vfcb = Vcb->VolumeFcb;
    PHPFS_CCB Ccb;
    NTSTATUS Status;

    if (CreateOptions & FILE_DIRECTORY_FILE) {
        return STATUS_NOT_A_DIRECTORY;
    }
    if (CreateDisposition != FILE_OPEN && CreateDisposition != FILE_OPEN_IF) {
        return STATUS_ACCESS_DENIED;
    }
    if (CreateOptions & FILE_DELETE_ON_CLOSE) {
        return STATUS_CANNOT_DELETE;
    }

    if (Vfcb->UncleanCount != 0) {
        Status = IoCheckShareAccess(DesiredAccess, ShareAccess, FileObject, &Vfcb->ShareAccess, TRUE);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    } else {
        IoSetShareAccess(DesiredAccess, ShareAccess, FileObject, &Vfcb->ShareAccess);
    }

    Ccb = HpfsCreateCcb();
    if (Ccb == NULL) {
        IoRemoveShareAccess(FileObject, &Vfcb->ShareAccess);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Ccb->Flags |= CCB_FLAG_VOLUME_OPEN;

    FileObject->FsContext = Vfcb;
    FileObject->FsContext2 = Ccb;
    FileObject->SectionObjectPointer = &Vfcb->SectionObjectPointers;
    FileObject->Vpb = Vcb->Vpb;

    Vfcb->RefCount++;
    Vfcb->UncleanCount++;
    Vcb->OpenCount++;
    Vcb->UncleanCount++;

    Ctx->Irp->IoStatus.Information = FILE_OPENED;
    return STATUS_SUCCESS;
}

static NTSTATUS
HpfsOpenFcb (
    PHPFS_IRP_CONTEXT Ctx,
    PHPFS_VCB Vcb,
    PHPFS_FCB Fcb,
    ACCESS_MASK DesiredAccess,
    USHORT ShareAccess,
    ULONG CreateOptions
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PHPFS_CCB Ccb;
    NTSTATUS Status;

    if (Fcb->UncleanCount != 0) {
        Status = IoCheckShareAccess(DesiredAccess, ShareAccess, FileObject, &Fcb->ShareAccess, TRUE);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    } else {
        IoSetShareAccess(DesiredAccess, ShareAccess, FileObject, &Fcb->ShareAccess);
    }

    Ccb = HpfsCreateCcb();
    if (Ccb == NULL) {
        IoRemoveShareAccess(FileObject, &Fcb->ShareAccess);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (CreateOptions & FILE_DELETE_ON_CLOSE) {
        Ccb->Flags |= CCB_FLAG_DELETE_ON_CLOSE;
    }
    if (DesiredAccess & HPFS_WRITE_ACCESS) {
        Ccb->Flags |= CCB_FLAG_WRITE_HANDLE;
        Vcb->WriteCount++;
    }

    FileObject->FsContext = Fcb;
    FileObject->FsContext2 = Ccb;
    FileObject->SectionObjectPointer = &Fcb->SectionObjectPointers;
    FileObject->Vpb = Vcb->Vpb;

    if (Ctx->IrpSp->Flags & SL_OPEN_PAGING_FILE) {
        Fcb->FcbState |= FCB_STATE_PAGING_FILE;
        Fcb->Header.IsFastIoPossible = FastIoIsNotPossible;
    }

    if (!(FileObject->Flags & FO_NO_INTERMEDIATE_BUFFERING) &&
        !(Fcb->FcbState & FCB_STATE_PAGING_FILE)) {
        FileObject->Flags |= FO_CACHE_SUPPORTED;
    }

    Fcb->RefCount++;
    Fcb->UncleanCount++;
    Vcb->OpenCount++;
    Vcb->UncleanCount++;

    if (FileObject->Flags & FO_NO_INTERMEDIATE_BUFFERING) {
        Fcb->NonCachedUncleanCount++;
    }
    if (HpfsIsFcb(Fcb)) {
        Fcb->Header.IsFastIoPossible = HpfsIsFastIoPossible(Fcb);
    }

    Ctx->Irp->IoStatus.Information = FILE_OPENED;
    return STATUS_SUCCESS;
}

static VOID
HpfsUndoOpen (
    PHPFS_IRP_CONTEXT Ctx,
    PHPFS_VCB Vcb,
    PHPFS_FCB Fcb
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PHPFS_CCB Ccb = (PHPFS_CCB)FileObject->FsContext2;

    IoRemoveShareAccess(FileObject, &Fcb->ShareAccess);
    if (Ccb->Flags & CCB_FLAG_WRITE_HANDLE) {
        Vcb->WriteCount--;
    }
    if (FileObject->Flags & FO_NO_INTERMEDIATE_BUFFERING) {
        Fcb->NonCachedUncleanCount--;
    }
    Fcb->UncleanCount--;
    Fcb->RefCount--;
    Vcb->OpenCount--;
    Vcb->UncleanCount--;
    HpfsDeleteCcb(Ccb);
    FileObject->FsContext = NULL;
    FileObject->FsContext2 = NULL;
}

/* The create's EA buffer, checked; NULL if none */
static NTSTATUS
HpfsCreateEaBuffer (
    PHPFS_IRP_CONTEXT Ctx,
    PFILE_FULL_EA_INFORMATION *Buffer,
    PULONG Length
    )
{
    PFILE_FULL_EA_INFORMATION Ea = (PFILE_FULL_EA_INFORMATION)Ctx->Irp->AssociatedIrp.SystemBuffer;
    ULONG EaLength = Ctx->IrpSp->Parameters.Create.EaLength;
    ULONG ErrorOffset;
    NTSTATUS Status;

    *Buffer = NULL;
    *Length = 0;
    if (Ea == NULL || EaLength == 0) {
        return STATUS_SUCCESS;
    }
    Status = HpfsCheckEaBuffer(Ea, EaLength, &ErrorOffset);
    if (!NT_SUCCESS(Status)) {
        Ctx->Irp->IoStatus.Information = ErrorOffset;
        return Status;
    }
    *Buffer = Ea;
    *Length = EaLength;
    return STATUS_SUCCESS;
}

/* FILE_SUPERSEDE, FILE_OVERWRITE and FILE_OVERWRITE_IF of an existing file */
static NTSTATUS
HpfsOverwriteFile (
    PHPFS_IRP_CONTEXT Ctx,
    PHPFS_VCB Vcb,
    PHPFS_FCB Fcb,
    ULONG CreateDisposition
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    UCHAR Attributes = (UCHAR)(Ctx->IrpSp->Parameters.Create.FileAttributes & HPFS_ATTR_SETTABLE);
    PFILE_FULL_EA_INFORMATION Ea;
    ULONG EaLength;
    LARGE_INTEGER Zero;
    NTSTATUS Status;

    Zero.QuadPart = 0;

    if (!MmCanFileBeTruncated(&Fcb->SectionObjectPointers, &Zero)) {
        return STATUS_USER_MAPPED_FILE;
    }

    Status = HpfsCreateEaBuffer(Ctx, &Ea, &EaLength);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);

    (VOID)CcPurgeCacheSection(&Fcb->SectionObjectPointers, NULL, 0, FALSE);

    Status = HpfsSetFileSize(Ctx, Fcb, FileObject, 0);

    if (NT_SUCCESS(Status)) {
        /* The file's EAs are replaced by the ones given (none: removed) */
        Status = HpfsSetEaBuffer(Vcb, Fcb, Ea, EaLength, TRUE);
    }

    if (NT_SUCCESS(Status)) {

        Attributes |= HPFS_ATTR_ARCHIVE;

        if (CreateDisposition == FILE_SUPERSEDE) {
            Fcb->Attributes = (UCHAR)(Attributes | (Fcb->Attributes & HPFS_ATTR_LONGNAME));
        } else {
            Fcb->Attributes |= Attributes;
        }

        KeQuerySystemTime(&Fcb->LastWriteTime);
        Fcb->LastAccessTime = Fcb->LastWriteTime;

        Fcb->FcbState |= FCB_STATE_DIRENT_DIRTY;
        Status = HpfsUpdateDirent(Vcb, Fcb);

        HpfsNotifyChange(Vcb, Fcb,
                         FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_ATTRIBUTES |
                         FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_EA,
                         FILE_ACTION_MODIFIED);
    }

    HpfsRelease(&Fcb->Resource);
    return Status;
}

/* Looks Component up in Dir: the entry, or STATUS_OBJECT_NAME_NOT_FOUND */
static NTSTATUS
HpfsLookupComponent (
    PHPFS_VCB Vcb,
    PHPFS_FCB Dir,
    const UCHAR *Oem,
    ULONG OemLength,
    PHPFS_ENTRY Entry
    )
{
    NTSTATUS Status;

    HpfsEngineEnter(Vcb);
    Status = HpfsLookup(&Vcb->Vol, Dir->Fnode, Oem, OemLength, Entry);
    Status = HpfsEngineLeave(Vcb, Status);

    if (Status == HPFS_NOT_FOUND) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    return Status;
}

/* Name was not found in Parent: make it */
static NTSTATUS
HpfsCreateNew (
    PHPFS_IRP_CONTEXT Ctx,
    PHPFS_VCB Vcb,
    PHPFS_FCB Parent,
    const UCHAR *Oem,
    ULONG OemLength,
    ULONG CreateDisposition,
    ULONG CreateOptions,
    USHORT ShareAccess,
    BOOLEAN TrailingBackslash,
    PHPFS_ENTRY Entry,
    PHPFS_FCB *Created
    )
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    ULONG FileAttributes = IrpSp->Parameters.Create.FileAttributes;
    BOOLEAN IsDirectory = (BOOLEAN)((CreateOptions & FILE_DIRECTORY_FILE) != 0);
    LONGLONG AllocationSize = Ctx->Irp->Overlay.AllocationSize.QuadPart;
    PFILE_FULL_EA_INFORMATION Ea;
    ULONG EaLength;
    ACCESS_MASK DesiredAccess;
    PHPFS_FCB Fcb = NULL;
    ULONG Now;
    NTSTATUS Status;

    *Created = NULL;

    if (Vcb->VcbState & VCB_STATE_READ_ONLY) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }
    if (Parent->FcbState & (FCB_STATE_DELETE_PENDING | FCB_STATE_DELETED)) {
        return STATUS_DELETE_PENDING;
    }
    if (IsDirectory && CreateDisposition != FILE_CREATE && CreateDisposition != FILE_OPEN_IF) {
        return STATUS_INVALID_PARAMETER;
    }
    if (TrailingBackslash && !IsDirectory) {
        return STATUS_OBJECT_NAME_INVALID;
    }

    Status = HpfsCreateEaBuffer(Ctx, &Ea, &EaLength);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    RtlZeroMemory(Entry, FIELD_OFFSET(HPFS_ENTRY, Name));
    RtlCopyMemory(Entry->Name, Oem, OemLength);
    Entry->Name[OemLength] = 0;
    Entry->NameLength = OemLength;
    Now = HpfsHostNow();
    Entry->WriteTime = Entry->AccessTime = Entry->CreateTime = Now;
    if (IsDirectory) {
        Entry->Attributes = (UCHAR)(FileAttributes & HPFS_ATTR_SETTABLE & ~HPFS_ATTR_ARCHIVE);
    } else {
        Entry->Attributes = (UCHAR)((FileAttributes & HPFS_ATTR_SETTABLE) | HPFS_ATTR_ARCHIVE);
    }

    HpfsEngineEnter(Vcb);
    Status = HpfsCreate(&Vcb->Vol, Parent->Fnode, Entry, IsDirectory);
    Status = HpfsEngineLeave(Vcb, Status);
    if (!NT_SUCCESS(Status)) {
        return (Status == HPFS_EXISTS) ? STATUS_OBJECT_NAME_COLLISION :
               (Status == HPFS_DISK_FULL) ? STATUS_DISK_FULL : Status;
    }

    Status = HpfsCreateFcb(Vcb, Parent, Entry, &Fcb);

    if (NT_SUCCESS(Status) && Ea != NULL) {
        Status = HpfsSetEaBuffer(Vcb, Fcb, Ea, EaLength, TRUE);
    }

    if (NT_SUCCESS(Status) && !IsDirectory && AllocationSize > 0) {
        Status = HpfsSetAllocation(Vcb, Fcb, (ULONGLONG)AllocationSize);
        if (NT_SUCCESS(Status)) {
            Fcb->FcbState |= FCB_STATE_TRUNCATE_ON_CLOSE;
        }
    }

    if (NT_SUCCESS(Status)) {
        DesiredAccess = HpfsResolveAccess(IrpSp, Vcb, NULL);
        Status = HpfsOpenFcb(Ctx, Vcb, Fcb, DesiredAccess, ShareAccess, CreateOptions);
    }

    if (!NT_SUCCESS(Status)) {
        if (Fcb != NULL) {
            (VOID)HpfsDeleteFromDisk(Vcb, Fcb);
            *Created = Fcb;
        } else {
            HpfsEngineEnter(Vcb);
            (VOID)HpfsEngineLeave(Vcb, HpfsDelete(&Vcb->Vol, Parent->Fnode, Oem, OemLength));
        }
        return Status;
    }

    *Created = Fcb;

    HpfsNotifyChange(Vcb, Fcb,
                     IsDirectory ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME,
                     FILE_ACTION_ADDED);

    Ctx->Irp->IoStatus.Information = FILE_CREATED;
    return STATUS_SUCCESS;
}

/* SL_OPEN_TARGET_DIRECTORY: open Parent and leave only the last component */
static NTSTATUS
HpfsOpenTargetDirectory (
    PHPFS_IRP_CONTEXT Ctx,
    PHPFS_VCB Vcb,
    PHPFS_FCB Parent,
    PUNICODE_STRING Name,
    const UCHAR *Oem,
    ULONG OemLength,
    ACCESS_MASK DesiredAccess,
    USHORT ShareAccess,
    PHPFS_ENTRY Entry
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    BOOLEAN Exists;
    NTSTATUS Status;

    if (Vcb->VcbState & VCB_STATE_READ_ONLY) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }

    Status = HpfsLookupComponent(Vcb, Parent, Oem, OemLength, Entry);
    if (!NT_SUCCESS(Status) && Status != STATUS_OBJECT_NAME_NOT_FOUND) {
        return Status;
    }
    Exists = (BOOLEAN)NT_SUCCESS(Status);

    Status = HpfsOpenFcb(Ctx, Vcb, Parent, DesiredAccess, ShareAccess, 0);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    RtlMoveMemory(FileObject->FileName.Buffer, Name->Buffer, Name->Length);
    FileObject->FileName.Length = Name->Length;

    Ctx->Irp->IoStatus.Information = Exists ? FILE_EXISTS : FILE_DOES_NOT_EXIST;
    return STATUS_SUCCESS;
}

NTSTATUS
HpfsCommonCreate (
    PHPFS_IRP_CONTEXT Ctx
    )
{
    PHPFS_VCB Vcb = Ctx->Vcb;
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PFILE_OBJECT RelatedFileObject = FileObject->RelatedFileObject;
    PHPFS_FCB RelatedFcb = NULL;
    PHPFS_FCB Current = NULL;
    PHPFS_FCB Child;
    PHPFS_FCB Created;
    UNICODE_STRING Remaining;
    UNICODE_STRING Component;
    ULONG CreateDisposition;
    ULONG CreateOptions;
    ACCESS_MASK DesiredAccess;
    ACCESS_MASK ShareCheckAccess;
    USHORT ShareAccess;
    BOOLEAN TrailingBackslash = FALSE;
    BOOLEAN OpenTarget;
    BOOLEAN Overwrite;
    BOOLEAN LastComponent;
    BOOLEAN Opened = FALSE;
    PHPFS_ENTRY Entry = NULL;
    UCHAR Oem[HPFS_MAX_NAME + 1];
    ULONG OemLength;
    NTSTATUS Status = STATUS_SUCCESS;

    CreateDisposition = (IrpSp->Parameters.Create.Options >> 24) & 0xFF;
    CreateOptions = IrpSp->Parameters.Create.Options & 0x00FFFFFF;
    ShareAccess = IrpSp->Parameters.Create.ShareAccess;
    OpenTarget = (BOOLEAN)((IrpSp->Flags & SL_OPEN_TARGET_DIRECTORY) != 0);

    Remaining = FileObject->FileName;

    if (RelatedFileObject != NULL) {
        RelatedFcb = (PHPFS_FCB)RelatedFileObject->FsContext;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Status = HpfsVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        if (Vcb->VcbState & (VCB_STATE_LOCKED | VCB_STATE_PNP_LOCKED)) {
            Status = STATUS_ACCESS_DENIED;
            __leave;
        }

        if (IrpSp->Flags & SL_OPEN_PAGING_FILE) {

            if (CreateOptions & FILE_DIRECTORY_FILE) {
                Status = STATUS_ACCESS_DENIED;
                __leave;
            }

            /* An existing paging file is opened as it is (Mm sizes it); a
               missing one is still created */
            if (CreateDisposition == FILE_SUPERSEDE || CreateDisposition == FILE_OVERWRITE_IF ||
                CreateDisposition == FILE_OPEN_IF) {
                CreateDisposition = FILE_OPEN_IF;
            } else if (CreateDisposition == FILE_OVERWRITE) {
                CreateDisposition = FILE_OPEN;
            }
        }

        if (CreateOptions & FILE_OPEN_BY_FILE_ID) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        if (CreateDisposition > FILE_OVERWRITE_IF ||
            ((CreateOptions & FILE_DIRECTORY_FILE) && (CreateOptions & FILE_NON_DIRECTORY_FILE))) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        /* The volume itself */
        if (Remaining.Length == 0 && (RelatedFcb == NULL || HpfsIsVfcb(RelatedFcb))) {

            if (OpenTarget) {
                Status = STATUS_INVALID_PARAMETER;
                __leave;
            }

            DesiredAccess = HpfsResolveAccess(IrpSp, Vcb, NULL);
            Status = HpfsOpenVolume(Ctx, Vcb, DesiredAccess, ShareAccess,
                                    CreateDisposition, CreateOptions);
            __leave;
        }

        if (RelatedFcb != NULL) {

            if (Remaining.Length != 0 && Remaining.Buffer[0] == L'\\') {
                Status = STATUS_OBJECT_NAME_INVALID;
                __leave;
            }

            if (HpfsIsVfcb(RelatedFcb)) {
                Current = Vcb->RootDcb;
            } else if (HpfsIsFcb(RelatedFcb) && Remaining.Length != 0) {
                Status = STATUS_OBJECT_PATH_NOT_FOUND;
                __leave;
            } else {
                Current = RelatedFcb;
            }

        } else {

            if (Remaining.Buffer[0] != L'\\') {
                Status = STATUS_OBJECT_NAME_INVALID;
                __leave;
            }

            Remaining.Buffer++;
            Remaining.Length -= sizeof(WCHAR);
            Current = Vcb->RootDcb;
        }

        if (Remaining.Length != 0 &&
            Remaining.Buffer[Remaining.Length / sizeof(WCHAR) - 1] == L'\\') {

            TrailingBackslash = TRUE;
            Remaining.Length -= sizeof(WCHAR);

            if (Remaining.Length == 0 ||
                Remaining.Buffer[Remaining.Length / sizeof(WCHAR) - 1] == L'\\') {
                Status = STATUS_OBJECT_NAME_INVALID;
                __leave;
            }
        }

        Remaining.MaximumLength = Remaining.Length;

        if (Remaining.Length == 0 && OpenTarget) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        Entry = (PHPFS_ENTRY)ExAllocatePoolWithTag(PagedPool, sizeof(HPFS_ENTRY), HPFS_TAG_BUFFER);
        if (Entry == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        /* Walk the path */
        while (HpfsNextComponent(&Remaining, &Component)) {

            LastComponent = (BOOLEAN)(Remaining.Length == 0);

            if (!HpfsIsValidUnicodeName(&Component)) {
                Status = STATUS_OBJECT_NAME_INVALID;
                __leave;
            }

            Status = HpfsUnicodeToOem(&Component, Oem, &OemLength);
            if (!NT_SUCCESS(Status) || !HpfsIsLegalName(Oem, OemLength)) {
                Status = STATUS_OBJECT_NAME_INVALID;
                __leave;
            }

            if (!HpfsIsDcb(Current)) {
                Status = STATUS_OBJECT_PATH_NOT_FOUND;
                __leave;
            }

            if (LastComponent && OpenTarget) {

                DesiredAccess = HpfsResolveAccess(IrpSp, Vcb, Current);
                Status = HpfsOpenTargetDirectory(Ctx, Vcb, Current, &Component, Oem, OemLength,
                                                 DesiredAccess, ShareAccess, Entry);
                if (NT_SUCCESS(Status)) {
                    Opened = TRUE;
                }
                __leave;
            }

            Status = HpfsLookupComponent(Vcb, Current, Oem, OemLength, Entry);

            if (Status == STATUS_OBJECT_NAME_NOT_FOUND) {

                if (!LastComponent) {
                    Status = STATUS_OBJECT_PATH_NOT_FOUND;
                    __leave;
                }

                if (CreateDisposition == FILE_OPEN || CreateDisposition == FILE_OVERWRITE) {
                    __leave;
                }

                Status = HpfsCreateNew(Ctx, Vcb, Current, Oem, OemLength, CreateDisposition,
                                       CreateOptions, ShareAccess, TrailingBackslash, Entry, &Created);

                if (NT_SUCCESS(Status)) {
                    Opened = TRUE;
                } else if (Created != NULL) {
                    Current = Created;
                }
                __leave;
            }

            if (!NT_SUCCESS(Status)) {
                __leave;
            }

            Child = HpfsFindFcbByFnode(Vcb, Entry->Fnode);

            if (Child == NULL) {
                Status = HpfsCreateFcb(Vcb, Current, Entry, &Child);
                if (!NT_SUCCESS(Status)) {
                    __leave;
                }
            }

            Current = Child;
        }

        /* Current exists */
        if (CreateDisposition == FILE_CREATE) {
            Status = STATUS_OBJECT_NAME_COLLISION;
            __leave;
        }

        if (HpfsIsDcb(Current)) {

            if (CreateOptions & FILE_NON_DIRECTORY_FILE) {
                Status = STATUS_FILE_IS_A_DIRECTORY;
                __leave;
            }
            if (CreateDisposition != FILE_OPEN && CreateDisposition != FILE_OPEN_IF) {
                Status = STATUS_OBJECT_NAME_COLLISION;
                __leave;
            }

        } else {

            if (CreateOptions & FILE_DIRECTORY_FILE) {
                Status = STATUS_NOT_A_DIRECTORY;
                __leave;
            }
            if (TrailingBackslash) {
                Status = STATUS_OBJECT_NAME_INVALID;
                __leave;
            }
        }

        if (Current->FcbState & FCB_STATE_DELETE_PENDING) {
            Status = STATUS_DELETE_PENDING;
            __leave;
        }

        /* A caller that does not understand EAs may not open a file that needs them */
        if ((CreateOptions & FILE_NO_EA_KNOWLEDGE) && (Current->DirentFlags & HPFS_DE_NEEDEA)) {
            Status = STATUS_ACCESS_DENIED;
            __leave;
        }

        DesiredAccess = HpfsResolveAccess(IrpSp, Vcb, Current);
        ShareCheckAccess = DesiredAccess;

        Overwrite = (BOOLEAN)(CreateDisposition == FILE_SUPERSEDE ||
                              CreateDisposition == FILE_OVERWRITE ||
                              CreateDisposition == FILE_OVERWRITE_IF);

        if (Overwrite) {

            ShareCheckAccess |= FILE_WRITE_DATA;

            if (((Current->Attributes & HPFS_ATTR_HIDDEN) &&
                 !(IrpSp->Parameters.Create.FileAttributes & FILE_ATTRIBUTE_HIDDEN)) ||
                ((Current->Attributes & HPFS_ATTR_SYSTEM) &&
                 !(IrpSp->Parameters.Create.FileAttributes & FILE_ATTRIBUTE_SYSTEM))) {
                Status = STATUS_ACCESS_DENIED;
                __leave;
            }
        }

        if ((ShareCheckAccess & HPFS_WRITE_ACCESS) || (CreateOptions & FILE_DELETE_ON_CLOSE)) {
            if (Vcb->VcbState & VCB_STATE_READ_ONLY) {
                Status = STATUS_MEDIA_WRITE_PROTECTED;
                __leave;
            }
        }

        if (HpfsIsFcb(Current) && (Current->Attributes & HPFS_ATTR_READONLY)) {
            if (ShareCheckAccess & HPFS_DATA_WRITE_ACCESS) {
                Status = STATUS_ACCESS_DENIED;
                __leave;
            }
            if (CreateOptions & FILE_DELETE_ON_CLOSE) {
                Status = STATUS_CANNOT_DELETE;
                __leave;
            }
        }

        if ((CreateOptions & FILE_DELETE_ON_CLOSE) && (Current->FcbState & FCB_STATE_ROOT)) {
            Status = STATUS_CANNOT_DELETE;
            __leave;
        }

        Status = HpfsOpenFcb(Ctx, Vcb, Current, ShareCheckAccess, ShareAccess, CreateOptions);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        if (Overwrite) {

            Status = HpfsOverwriteFile(Ctx, Vcb, Current, CreateDisposition);

            if (!NT_SUCCESS(Status)) {
                HpfsUndoOpen(Ctx, Vcb, Current);
                __leave;
            }

            Ctx->Irp->IoStatus.Information =
                (CreateDisposition == FILE_SUPERSEDE) ? FILE_SUPERSEDED : FILE_OVERWRITTEN;
        }

        Opened = TRUE;

    } __finally {

        if (!Opened || AbnormalTermination()) {
            if (Current != NULL && Current != RelatedFcb) {
                HpfsDropUnopened(Vcb, Current);
            }
        }

        if (Entry != NULL) {
            ExFreePool(Entry);
        }

        HpfsRelease(&Vcb->Resource);
    }

    return Status;
}
