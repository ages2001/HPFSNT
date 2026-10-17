/*
 * HPFSNT - IRP_MJ_DIRECTORY_CONTROL
 *
 * An enumeration walks the directory's dnode tree in name order. HPFS gives
 * an entry no stable position, so a handle remembers the last name it
 * returned and continues with the first one that sorts after it; entries
 * added or removed in between are seen or not as their names fall. "." and
 * ".." come first (not in the root), as NT expects.
 */

#include "hpfs.h"

/* ------------------------------------------------------------------ */
/* IRP_MN_QUERY_DIRECTORY                                              */
/* ------------------------------------------------------------------ */

typedef struct _HPFS_LIST_ENTRY {
    LARGE_INTEGER   CreationTime;
    LARGE_INTEGER   LastAccessTime;
    LARGE_INTEGER   LastWriteTime;
    LARGE_INTEGER   EndOfFile;
    LARGE_INTEGER   AllocationSize;
    ULONG           Attributes;
    ULONG           EaSize;
    UNICODE_STRING  Name;
} HPFS_LIST_ENTRY, *PHPFS_LIST_ENTRY;

static VOID
HpfsListFromDcb (
    PHPFS_FCB Dcb,
    PHPFS_LIST_ENTRY List,
    PWSTR Name,
    USHORT NameLength
    )
{
    RtlZeroMemory(List, sizeof(HPFS_LIST_ENTRY));

    if (Dcb != NULL) {
        List->CreationTime = Dcb->CreationTime;
        List->LastAccessTime = Dcb->LastAccessTime;
        List->LastWriteTime = Dcb->LastWriteTime;
    }

    List->Attributes = FILE_ATTRIBUTE_DIRECTORY;
    List->Name.Buffer = Name;
    List->Name.Length = NameLength;
    List->Name.MaximumLength = NameLength;
}

/* The NT EA size of a directory entry (reads the EAs only when it has some) */
static ULONG
HpfsEntryEaSize (
    PHPFS_VCB Vcb,
    PHPFS_ENTRY Entry
    )
{
    if (!(Entry->Flags & HPFS_DE_EA) && Entry->EaSize == 0) {
        return 0;
    }
    return HpfsNtEaSizeOfFnode(Vcb, Entry->Fnode);
}

static VOID
HpfsListFromEntry (
    PHPFS_VCB Vcb,
    PHPFS_ENTRY Entry,
    PHPFS_LIST_ENTRY List,
    BOOLEAN WantEaSize
    )
{
    PHPFS_FCB Fcb;

    RtlZeroMemory(List, FIELD_OFFSET(HPFS_LIST_ENTRY, Name));

    /* An open file's entry may lag behind its FCB */
    Fcb = HpfsFindFcbByFnode(Vcb, Entry->Fnode);

    if (Fcb != NULL) {
        List->CreationTime = Fcb->CreationTime;
        List->LastAccessTime = Fcb->LastAccessTime;
        List->LastWriteTime = Fcb->LastWriteTime;
        List->Attributes = HpfsNtAttributes(Fcb->Attributes);
        if (HpfsIsFcb(Fcb)) {
            List->EndOfFile = Fcb->Header.FileSize;
            List->AllocationSize = Fcb->Header.AllocationSize;
        }
    } else {
        List->CreationTime = HpfsToNtTime(Entry->CreateTime);
        List->LastAccessTime = HpfsToNtTime(Entry->AccessTime);
        List->LastWriteTime = HpfsToNtTime(Entry->WriteTime);
        List->Attributes = HpfsNtAttributes(Entry->Attributes);
        if (!(Entry->Attributes & HPFS_ATTR_DIRECTORY)) {
            List->EndOfFile.QuadPart = Entry->FileSize;
            List->AllocationSize.QuadPart = HpfsRoundUp((LONGLONG)Entry->FileSize, HPFS_SECTOR_SIZE);
        }
    }

    if (WantEaSize) {
        List->EaSize = HpfsEntryEaSize(Vcb, Entry);
    }
}

static ULONG
HpfsListBaseLength (
    FILE_INFORMATION_CLASS Class
    )
{
    switch (Class) {
    case FileDirectoryInformation:
        return FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName);
    case FileFullDirectoryInformation:
        return FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName);
    case FileBothDirectoryInformation:
        return FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName);
    case FileNamesInformation:
        return FIELD_OFFSET(FILE_NAMES_INFORMATION, FileName);
    default:
        return 0;
    }
}

static ULONG
HpfsFillListEntry (
    FILE_INFORMATION_CLASS Class,
    PUCHAR Buffer,
    ULONG Room,
    PHPFS_LIST_ENTRY List
    )
{
    PFILE_DIRECTORY_INFORMATION Dir;
    PFILE_FULL_DIR_INFORMATION Full;
    PFILE_BOTH_DIR_INFORMATION Both;
    PFILE_NAMES_INFORMATION Names;
    ULONG Base = HpfsListBaseLength(Class);
    ULONG Copy;

    RtlZeroMemory(Buffer, Base);

    Copy = List->Name.Length;
    if (Copy > Room - Base) {
        Copy = Room - Base;
    }

    switch (Class) {

    case FileDirectoryInformation:
        Dir = (PFILE_DIRECTORY_INFORMATION)Buffer;
        Dir->CreationTime = List->CreationTime;
        Dir->LastAccessTime = List->LastAccessTime;
        Dir->LastWriteTime = List->LastWriteTime;
        Dir->ChangeTime = List->LastWriteTime;
        Dir->EndOfFile = List->EndOfFile;
        Dir->AllocationSize = List->AllocationSize;
        Dir->FileAttributes = List->Attributes;
        Dir->FileNameLength = List->Name.Length;
        RtlCopyMemory(Dir->FileName, List->Name.Buffer, Copy);
        break;

    case FileFullDirectoryInformation:
        Full = (PFILE_FULL_DIR_INFORMATION)Buffer;
        Full->CreationTime = List->CreationTime;
        Full->LastAccessTime = List->LastAccessTime;
        Full->LastWriteTime = List->LastWriteTime;
        Full->ChangeTime = List->LastWriteTime;
        Full->EndOfFile = List->EndOfFile;
        Full->AllocationSize = List->AllocationSize;
        Full->FileAttributes = List->Attributes;
        Full->EaSize = List->EaSize;
        Full->FileNameLength = List->Name.Length;
        RtlCopyMemory(Full->FileName, List->Name.Buffer, Copy);
        break;

    case FileBothDirectoryInformation:
        Both = (PFILE_BOTH_DIR_INFORMATION)Buffer;
        Both->CreationTime = List->CreationTime;
        Both->LastAccessTime = List->LastAccessTime;
        Both->LastWriteTime = List->LastWriteTime;
        Both->ChangeTime = List->LastWriteTime;
        Both->EndOfFile = List->EndOfFile;
        Both->AllocationSize = List->AllocationSize;
        Both->FileAttributes = List->Attributes;
        Both->EaSize = List->EaSize;
        Both->FileNameLength = List->Name.Length;
        Both->ShortNameLength = 0;
        RtlCopyMemory(Both->FileName, List->Name.Buffer, Copy);
        break;

    case FileNamesInformation:
        Names = (PFILE_NAMES_INFORMATION)Buffer;
        Names->FileNameLength = List->Name.Length;
        RtlCopyMemory(Names->FileName, List->Name.Buffer, Copy);
        break;

    default:
        break;
    }

    return Copy;
}

static BOOLEAN
HpfsListMatches (
    PHPFS_CCB Ccb,
    PUNICODE_STRING Name
    )
{
    if (Ccb->Flags & CCB_FLAG_MATCH_ALL) {
        return TRUE;
    }

    if (Ccb->Flags & CCB_FLAG_WILDCARD) {
        return FsRtlIsNameInExpression(&Ccb->Pattern, Name, TRUE, NULL);
    }

    return RtlEqualUnicodeString(&Ccb->Pattern, Name, TRUE);
}

static NTSTATUS
HpfsSetPattern (
    PHPFS_CCB Ccb,
    PUNICODE_STRING FileName
    )
{
    UNICODE_STRING Pattern;
    NTSTATUS Status;

    if (Ccb->Pattern.Buffer != NULL) {
        ExFreePool(Ccb->Pattern.Buffer);
        RtlZeroMemory(&Ccb->Pattern, sizeof(UNICODE_STRING));
    }

    Ccb->Flags &= ~(CCB_FLAG_WILDCARD | CCB_FLAG_MATCH_ALL);

    if (FileName == NULL || FileName->Length == 0 ||
        (FileName->Length == sizeof(WCHAR) && FileName->Buffer[0] == L'*')) {

        Ccb->Flags |= CCB_FLAG_MATCH_ALL | CCB_FLAG_PATTERN_SET;
        return STATUS_SUCCESS;
    }

    Pattern.Buffer = (PWSTR)ExAllocatePoolWithTag(PagedPool, FileName->Length, HPFS_TAG_NAME);
    if (Pattern.Buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Pattern.Length = 0;
    Pattern.MaximumLength = FileName->Length;

    Status = RtlUpcaseUnicodeString(&Pattern, FileName, FALSE);
    if (!NT_SUCCESS(Status)) {
        ExFreePool(Pattern.Buffer);
        return Status;
    }

    Ccb->Pattern = Pattern;

    if (FsRtlDoesNameContainWildCards(&Ccb->Pattern)) {
        Ccb->Flags |= CCB_FLAG_WILDCARD;
    }

    Ccb->Flags |= CCB_FLAG_PATTERN_SET;
    return STATUS_SUCCESS;
}

static NTSTATUS
HpfsQueryDirectory (
    PHPFS_IRP_CONTEXT Ctx,
    PHPFS_FCB Dcb,
    PHPFS_CCB Ccb
    )
{
    PHPFS_VCB Vcb = Dcb->Vcb;
    PIRP Irp = Ctx->Irp;
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    FILE_INFORMATION_CLASS Class;
    PUNICODE_STRING FileName;
    ULONG Length;
    PUCHAR Buffer;
    PUCHAR Out = NULL;
    ULONG Base;
    ULONG EntryLength;
    ULONG Room;
    ULONG Copied;
    ULONG NextOffset = 0;
    ULONG LastOffset = 0;
    ULONG Returned = 0;
    ULONG Information = 0;
    BOOLEAN InitialQuery = FALSE;
    BOOLEAN IsRoot = (BOOLEAN)(Dcb->ParentDcb == NULL);
    BOOLEAN WantEaSize;
    BOOLEAN FromEntry;
    BOOLEAN Matched;
    HPFS_LIST_ENTRY List;
    PHPFS_ENTRY Entry = NULL;
    UNICODE_STRING Name;
    NTSTATUS Status = STATUS_SUCCESS;

    RtlZeroMemory(&Name, sizeof(Name));

    Class    = HpfsXSp(IrpSp)->Parameters.QueryDirectory.FileInformationClass;
    FileName = (PUNICODE_STRING)HpfsXSp(IrpSp)->Parameters.QueryDirectory.FileName;
    Length   = HpfsXSp(IrpSp)->Parameters.QueryDirectory.Length;

    Base = HpfsListBaseLength(Class);
    if (Base == 0) {
        return STATUS_INVALID_INFO_CLASS;
    }

    if (Length < Base) {
        return STATUS_INFO_LENGTH_MISMATCH;
    }

    WantEaSize = (BOOLEAN)(Class == FileFullDirectoryInformation ||
                           Class == FileBothDirectoryInformation);

    Buffer = (PUCHAR)HpfsMapUserBuffer(Irp);
    if (Buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Built here and copied out at the end: the caller's buffer may fault */
    Out = (PUCHAR)ExAllocatePoolWithTag(PagedPool, Length, HPFS_TAG_BUFFER);
    Entry = (PHPFS_ENTRY)ExAllocatePoolWithTag(PagedPool, sizeof(HPFS_ENTRY), HPFS_TAG_BUFFER);
    if (Out == NULL || Entry == NULL) {
        if (Out != NULL) ExFreePool(Out);
        if (Entry != NULL) ExFreePool(Entry);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    (VOID)ExAcquireResourceSharedLite(&Vcb->Resource, TRUE);
    (VOID)ExAcquireResourceExclusiveLite(&Dcb->Resource, TRUE);

    __try {

        Status = HpfsVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        if (Dcb->FcbState & FCB_STATE_DELETED) {
            Status = STATUS_FILE_DELETED;
            __leave;
        }

        if (!(Ccb->Flags & CCB_FLAG_PATTERN_SET) ||
            ((IrpSp->Flags & SL_RESTART_SCAN) && FileName != NULL && FileName->Length != 0)) {

            /* The caller's name lives in its buffer */
            UNICODE_STRING Captured;
            RtlZeroMemory(&Captured, sizeof(Captured));
            Status = STATUS_SUCCESS;

            if (FileName != NULL && FileName->Length != 0) {
                Captured.Buffer = (PWSTR)ExAllocatePoolWithTag(PagedPool, FileName->Length, HPFS_TAG_NAME);
                if (Captured.Buffer == NULL) {
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    __leave;
                }
                Status = HpfsSafeCopy(Captured.Buffer, FileName->Buffer, FileName->Length);
                if (NT_SUCCESS(Status)) {
                    Captured.Length = Captured.MaximumLength = FileName->Length;
                }
            }

            if (NT_SUCCESS(Status)) {
                Status = HpfsSetPattern(Ccb, Captured.Length ? &Captured : NULL);
            }
            if (Captured.Buffer != NULL) {
                ExFreePool(Captured.Buffer);
            }
            if (!NT_SUCCESS(Status)) {
                __leave;
            }

            InitialQuery = TRUE;
            Ccb->QueryState = 0;
            Ccb->HaveLast = FALSE;
        }

        if (IrpSp->Flags & SL_RESTART_SCAN) {
            Ccb->QueryState = 0;
            Ccb->HaveLast = FALSE;
        }

        if (IsRoot && Ccb->QueryState < 2) {
            Ccb->QueryState = 2;
        }

        for (;;) {

            FromEntry = FALSE;

            if (Ccb->QueryState == 0) {

                HpfsListFromDcb(Dcb, &List, L".", sizeof(WCHAR));

            } else if (Ccb->QueryState == 1) {

                HpfsListFromDcb(Dcb->ParentDcb, &List, L"..", 2 * sizeof(WCHAR));

            } else {

                HpfsEngineEnter(Vcb);
                Status = HpfsNextEntry(&Vcb->Vol, Dcb->Fnode,
                                       Ccb->HaveLast ? Ccb->LastName : NULL,
                                       Ccb->HaveLast ? Ccb->LastNameLength : 0,
                                       Entry);
                Status = HpfsEngineLeave(Vcb, Status);

                if (Status == STATUS_NO_MORE_ENTRIES) {
                    Status = STATUS_SUCCESS;
                    break;
                }
                if (!NT_SUCCESS(Status)) {
                    break;
                }

                if (Name.Buffer != NULL) {
                    ExFreePool(Name.Buffer);
                    Name.Buffer = NULL;
                }
                Status = HpfsOemToUnicode(Entry->Name, Entry->NameLength, &Name);
                if (!NT_SUCCESS(Status)) {
                    break;
                }

                List.Name = Name;
                FromEntry = TRUE;
            }

            Matched = HpfsListMatches(Ccb, &List.Name);
            if (!Matched) {
                goto Consumed;
            }

            if (FromEntry) {
                HpfsListFromEntry(Vcb, Entry, &List, WantEaSize);
            }

            EntryLength = Base + List.Name.Length;
            Room = Length - NextOffset;

            if (NextOffset + Base > Length || (Returned != 0 && EntryLength > Room)) {
                /* Not consumed: the next call starts here */
                if (Returned == 0) {
                    Status = STATUS_BUFFER_OVERFLOW;
                }
                break;
            }

            if (Returned != 0) {
                *(PULONG)(Out + LastOffset) = NextOffset - LastOffset;
            }

            Copied = HpfsFillListEntry(Class, Out + NextOffset, Room, &List);

            Returned++;
            LastOffset = NextOffset;
            Information = NextOffset + Base + Copied;

            if (Copied < List.Name.Length) {
                /* Partly returned; NT moves on all the same */
                Status = STATUS_BUFFER_OVERFLOW;
            }

        Consumed:
            if (Ccb->QueryState < 2) {
                Ccb->QueryState++;
            } else {
                RtlCopyMemory(Ccb->LastName, Entry->Name, Entry->NameLength);
                Ccb->LastNameLength = Entry->NameLength;
                Ccb->HaveLast = TRUE;
            }

            if (!Matched) {
                continue;
            }

            if (Status == STATUS_BUFFER_OVERFLOW) {
                break;
            }

            if (IrpSp->Flags & SL_RETURN_SINGLE_ENTRY) {
                break;
            }

            NextOffset = (Information + 7) & ~7UL;

            if (NextOffset >= Length) {
                break;
            }
        }

        if (Returned == 0 && NT_SUCCESS(Status)) {
            Status = InitialQuery ? STATUS_NO_SUCH_FILE : STATUS_NO_MORE_FILES;
        }

        if (Returned != 0) {
            NTSTATUS Copy = HpfsSafeCopy(Buffer, Out, Information);
            if (!NT_SUCCESS(Copy)) {
                Status = Copy;
                Information = 0;
            }
        }

        Irp->IoStatus.Information = Information;

    } __finally {

        if (Name.Buffer != NULL) {
            ExFreePool(Name.Buffer);
        }
        ExFreePool(Entry);
        ExFreePool(Out);
        HpfsRelease(&Dcb->Resource);
        HpfsRelease(&Vcb->Resource);
    }

    return Status;
}

/* ------------------------------------------------------------------ */
/* IRP_MN_NOTIFY_CHANGE_DIRECTORY                                      */
/* ------------------------------------------------------------------ */

#ifdef HPFS_NT31

/*
 * NT 3.1 does not export the FsRtlNotifyFull API, so a directory-change
 * notify request cannot be handed to the FsRtl notify package. Completing it
 * at once makes the caller (File Manager) re-read the directory and re-arm
 * the notify in a tight loop. Instead the request is held pending on the
 * volume's list and completed when its handle is cleaned up or it is
 * cancelled. (No live notification; a manual refresh works.)
 */
static VOID NTAPI
HpfsCancelNotifyIrp (
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp
    )
{
    UNREFERENCED_PARAMETER(DeviceObject);

    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    IoReleaseCancelSpinLock(Irp->CancelIrql);

    Irp->IoStatus.Status = STATUS_CANCELLED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

VOID
HpfsNotifyCleanupList (
    PHPFS_VCB Vcb,
    PHPFS_CCB Ccb
    )
{
    KIRQL Irql;
    LIST_ENTRY Done;
    PLIST_ENTRY Entry, Next;
    PIRP Nirp;
    PIO_STACK_LOCATION Nsp;

    InitializeListHead(&Done);

    IoAcquireCancelSpinLock(&Irql);
    for (Entry = Vcb->DirNotifyList.Flink; Entry != &Vcb->DirNotifyList; Entry = Next) {
        Next = Entry->Flink;
        Nirp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
        Nsp = IoGetCurrentIrpStackLocation(Nirp);

        if (Ccb == NULL || Nsp->FileObject->FsContext2 == Ccb) {
            Nirp->CancelRoutine = NULL;
            RemoveEntryList(Entry);
            InsertTailList(&Done, Entry);
        }
    }
    IoReleaseCancelSpinLock(Irql);

    while (!IsListEmpty(&Done)) {
        Entry = RemoveHeadList(&Done);
        Nirp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
        Nirp->IoStatus.Status = STATUS_NOTIFY_CLEANUP;
        Nirp->IoStatus.Information = 0;
        IoCompleteRequest(Nirp, IO_NO_INCREMENT);
    }
}

#else

VOID
HpfsNotifyCleanupList (
    PHPFS_VCB Vcb,
    PHPFS_CCB Ccb
    )
{
    FsRtlNotifyCleanup(Vcb->NotifySync, &Vcb->DirNotifyList, Ccb);
}

#endif /* HPFS_NT31 */

static NTSTATUS
HpfsNotifyChangeDirectory (
    PHPFS_IRP_CONTEXT Ctx,
    PHPFS_FCB Dcb,
    PHPFS_CCB Ccb
    )
{
    PHPFS_VCB Vcb = Dcb->Vcb;
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    NTSTATUS Status;
#ifdef HPFS_NT31
    KIRQL Irql;
    BOOLEAN Cancelled = FALSE;
#endif

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Status = HpfsVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

#ifdef HPFS_NT31
        UNREFERENCED_PARAMETER(IrpSp);
        UNREFERENCED_PARAMETER(Ccb);

        IoMarkIrpPending(Ctx->Irp);
        IoAcquireCancelSpinLock(&Irql);
        if (Ctx->Irp->Cancel) {
            IoReleaseCancelSpinLock(Irql);
            Cancelled = TRUE;
        } else {
            InsertTailList(&Vcb->DirNotifyList, &Ctx->Irp->Tail.Overlay.ListEntry);
            Ctx->Irp->CancelRoutine = HpfsCancelNotifyIrp;
            IoReleaseCancelSpinLock(Irql);
        }
        Ctx->Flags |= HPFS_CTX_NO_COMPLETE;
        Status = STATUS_PENDING;
#else
        Status = HpfsBuildFullName(Dcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        FsRtlNotifyFullChangeDirectory(Vcb->NotifySync,
                                       &Vcb->DirNotifyList,
                                       Ccb,
                                       (PSTRING)&Dcb->FullName,
                                       (BOOLEAN)((IrpSp->Flags & SL_WATCH_TREE) != 0),
                                       FALSE,
                                       HpfsXSp(IrpSp)->Parameters.NotifyDirectory.CompletionFilter,
                                       Ctx->Irp,
                                       NULL,
                                       NULL);

        Ctx->Flags |= HPFS_CTX_NO_COMPLETE;
        Status = STATUS_PENDING;
#endif

    } __finally {

        HpfsRelease(&Vcb->Resource);
    }

#ifdef HPFS_NT31
    if (Cancelled) {
        Ctx->Irp->IoStatus.Status = STATUS_CANCELLED;
        Ctx->Irp->IoStatus.Information = 0;
        IoCompleteRequest(Ctx->Irp, IO_NO_INCREMENT);
    }
#endif

    return Status;
}

VOID
HpfsNotifyChange (
    PHPFS_VCB Vcb,
    PHPFS_FCB Fcb,
    ULONG Filter,
    ULONG Action
    )
{
#ifdef HPFS_NT31
    UNREFERENCED_PARAMETER(Vcb);
    UNREFERENCED_PARAMETER(Fcb);
    UNREFERENCED_PARAMETER(Filter);
    UNREFERENCED_PARAMETER(Action);
#else
    if (Fcb->ParentDcb == NULL || !NT_SUCCESS(HpfsBuildFullName(Fcb))) {
        return;
    }

    FsRtlNotifyFullReportChange(Vcb->NotifySync,
                                &Vcb->DirNotifyList,
                                (PSTRING)&Fcb->FullName,
                                (USHORT)(Fcb->FullName.Length - Fcb->Name.Length),
                                NULL,
                                NULL,
                                Filter,
                                Action,
                                NULL);
#endif
}

NTSTATUS
HpfsCommonDirectoryControl (
    PHPFS_IRP_CONTEXT Ctx
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PHPFS_FCB Dcb = (PHPFS_FCB)FileObject->FsContext;
    PHPFS_CCB Ccb = (PHPFS_CCB)FileObject->FsContext2;

    if (Dcb == NULL || Ccb == NULL || !HpfsIsDcb(Dcb)) {
        return STATUS_INVALID_PARAMETER;
    }

    switch (Ctx->IrpSp->MinorFunction) {

    case IRP_MN_QUERY_DIRECTORY:
        return HpfsQueryDirectory(Ctx, Dcb, Ccb);

    case IRP_MN_NOTIFY_CHANGE_DIRECTORY:
        return HpfsNotifyChangeDirectory(Ctx, Dcb, Ccb);
    }

    return STATUS_INVALID_DEVICE_REQUEST;
}
