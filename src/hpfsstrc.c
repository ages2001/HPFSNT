/*
 * HPFSNT - FCBs, CCBs, names, times and the glue to the engine
 *
 * A file has one FCB, found by its fnode (HPFS's file identity), so every
 * open of a file - under any spelling of its name - shares one set of
 * section object pointers. An FCB holds a reference on its parent.
 */

#include "hpfs.h"

/* ------------------------------------------------------------------ */
/* Times: HPFS keeps local time, seconds since 1970                    */
/* ------------------------------------------------------------------ */

LARGE_INTEGER
HpfsToNtTime (
    ULONG Seconds
    )
{
    LARGE_INTEGER Time;

    if (Seconds == 0) {
        Time.QuadPart = 0;
        return Time;
    }
    RtlSecondsSince1970ToTime(Seconds, &Time);
    ExLocalTimeToSystemTime(&Time, &Time);
    return Time;
}

ULONG
HpfsFromNtTime (
    LARGE_INTEGER Time
    )
{
    LARGE_INTEGER Local;
    ULONG Seconds;

    if (Time.QuadPart == 0) {
        return 0;
    }
    ExSystemTimeToLocalTime(&Time, &Local);
    if (!RtlTimeToSecondsSince1970(&Local, &Seconds)) {
        /* before 1970 or after 2106: clamp */
        return (Local.QuadPart < 0x019DB1DED53E8000LL) ? 1 : 0xFFFFFFFF;
    }
    return Seconds;
}

/* ------------------------------------------------------------------ */
/* Names: NT (Unicode) <-> HPFS (OEM code page)                        */
/* ------------------------------------------------------------------ */

BOOLEAN
HpfsIsValidUnicodeName (
    PUNICODE_STRING Name
    )
{
    USHORT Count = Name->Length / sizeof(WCHAR);
    USHORT i;
    WCHAR C;

    if (Count == 0 || Count > HPFS_MAX_NAME || (Name->Length & 1) != 0) {
        return FALSE;
    }
    if ((Count == 1 && Name->Buffer[0] == L'.') ||
        (Count == 2 && Name->Buffer[0] == L'.' && Name->Buffer[1] == L'.')) {
        return FALSE;
    }
    for (i = 0; i < Count; i++) {
        C = Name->Buffer[i];
        if (C < 0x20 || C == L'"' || C == L'*' || C == L'/' || C == L':' || C == L'<' ||
            C == L'>' || C == L'?' || C == L'\\' || C == L'|') {
            return FALSE;
        }
    }
    if (Name->Buffer[Count - 1] == L'.' || Name->Buffer[Count - 1] == L' ') {
        return FALSE;
    }
    return TRUE;
}

/*
 * The OEM form of a name. Fails for a character the code page cannot
 * hold (it would come back as another character).
 */
NTSTATUS
HpfsUnicodeToOem (
    PCUNICODE_STRING Name,
    PUCHAR Oem,
    PULONG OemLength
    )
{
    WCHAR Back[HPFS_MAX_NAME];
    ULONG Bytes = 0, BackBytes = 0;
    NTSTATUS Status;

    if (Name->Length == 0 || Name->Length > HPFS_MAX_NAME * sizeof(WCHAR)) {
        return STATUS_OBJECT_NAME_INVALID;
    }
    Status = RtlUnicodeToOemN((PCHAR)Oem, HPFS_MAX_NAME, &Bytes, Name->Buffer, Name->Length);
    if (!NT_SUCCESS(Status) || Bytes == 0 || Bytes > HPFS_MAX_NAME) {
        return STATUS_OBJECT_NAME_INVALID;
    }
    Status = RtlOemToUnicodeN(Back, sizeof(Back), &BackBytes, (PCHAR)Oem, Bytes);
    if (!NT_SUCCESS(Status) || BackBytes != Name->Length ||
        RtlCompareMemory(Back, Name->Buffer, Name->Length) != Name->Length) {
        return STATUS_OBJECT_NAME_INVALID;
    }
    *OemLength = Bytes;
    return STATUS_SUCCESS;
}

/* Allocates Name->Buffer (paged pool) */
NTSTATUS
HpfsOemToUnicode (
    const UCHAR *Oem,
    ULONG OemLength,
    PUNICODE_STRING Name
    )
{
    ULONG Bytes = 0;
    NTSTATUS Status;

    Name->Buffer = (PWSTR)ExAllocatePoolWithTag(PagedPool, (OemLength + 1) * sizeof(WCHAR), HPFS_TAG_NAME);
    if (Name->Buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Status = RtlOemToUnicodeN(Name->Buffer, OemLength * sizeof(WCHAR), &Bytes, (PCHAR)Oem, OemLength);
    if (!NT_SUCCESS(Status)) {
        ExFreePool(Name->Buffer);
        Name->Buffer = NULL;
        return Status;
    }
    Name->Length = (USHORT)Bytes;
    Name->MaximumLength = (USHORT)((OemLength + 1) * sizeof(WCHAR));
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* FCBs                                                                */
/* ------------------------------------------------------------------ */

static PHPFS_FCB
HpfsAllocateFcb (
    PHPFS_VCB Vcb,
    CSHORT NodeType
    )
{
    PHPFS_FCB Fcb;

    Fcb = (PHPFS_FCB)ExAllocatePoolWithTag(NonPagedPool, sizeof(HPFS_FCB), HPFS_TAG_FCB);
    if (Fcb == NULL) {
        return NULL;
    }

    RtlZeroMemory(Fcb, sizeof(HPFS_FCB));

    Fcb->Header.NodeTypeCode = NodeType;
    Fcb->Header.NodeByteSize = (CSHORT)sizeof(HPFS_FCB);
    Fcb->Header.IsFastIoPossible = FastIoIsNotPossible;

    ExInitializeResourceLite(&Fcb->Resource);
    ExInitializeResourceLite(&Fcb->PagingIoResource);

    Fcb->Header.Resource = &Fcb->Resource;
    Fcb->Header.PagingIoResource = &Fcb->PagingIoResource;

    FsRtlInitializeFileLock(&Fcb->FileLock, NULL, NULL);

    Fcb->Vcb = Vcb;
    InitializeListHead(&Fcb->FcbLinks);

    return Fcb;
}

VOID
HpfsDeleteFcb (
    PHPFS_FCB Fcb
    )
{
    RemoveEntryList(&Fcb->FcbLinks);

    FsRtlUninitializeFileLock(&Fcb->FileLock);

    ExDeleteResourceLite(&Fcb->Resource);
    ExDeleteResourceLite(&Fcb->PagingIoResource);

    if (Fcb->Name.Buffer != NULL) {
        ExFreePool(Fcb->Name.Buffer);
    }
    if (Fcb->FullName.Buffer != NULL) {
        ExFreePool(Fcb->FullName.Buffer);
    }
    if (Fcb->Runs != NULL) {
        ExFreePool(Fcb->Runs);
    }

    ExFreePool(Fcb);
}

PHPFS_FCB
HpfsCreateVolumeFcb (
    PHPFS_VCB Vcb
    )
{
    PHPFS_FCB Fcb;

    Fcb = HpfsAllocateFcb(Vcb, HPFS_NTC_VFCB);
    if (Fcb == NULL) {
        return NULL;
    }

    Fcb->Header.AllocationSize.QuadPart = Vcb->PartitionBytes;
    Fcb->Header.FileSize.QuadPart = Vcb->PartitionBytes;
    Fcb->Header.ValidDataLength.QuadPart = Vcb->PartitionBytes;

    return Fcb;
}

PHPFS_FCB
HpfsCreateRootDcb (
    PHPFS_VCB Vcb
    )
{
    PHPFS_FCB Dcb;

    Dcb = HpfsAllocateFcb(Vcb, HPFS_NTC_DCB);
    if (Dcb == NULL) {
        return NULL;
    }

    Dcb->FcbState = FCB_STATE_ROOT;
    Dcb->Attributes = HPFS_ATTR_DIRECTORY;
    Dcb->Fnode = Vcb->Vol.RootFnode;
    Dcb->DirFnode = Vcb->Vol.RootFnode;

    InsertTailList(&Vcb->FcbList, &Dcb->FcbLinks);
    return Dcb;
}

/* Reads the run list of a file into its FCB (engine not held by the caller) */
NTSTATUS
HpfsLoadRuns (
    PHPFS_VCB Vcb,
    PHPFS_FCB Fcb
    )
{
    PHPFS_RUN Runs = NULL, Copy = NULL;
    ULONG Count = 0, Sectors = 0;
    NTSTATUS Status;

    HpfsEngineEnter(Vcb);
    Status = HpfsGetRuns(&Vcb->Vol, Fcb->Fnode, &Runs, &Count, &Sectors);
    Status = HpfsEngineLeave(Vcb, Status);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    if (Count != 0) {
        Copy = (PHPFS_RUN)ExAllocatePoolWithTag(NonPagedPool, Count * sizeof(HPFS_RUN), HPFS_TAG_RUNS);
        if (Copy == NULL) {
            HpfsHostFree(Runs);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlCopyMemory(Copy, Runs, Count * sizeof(HPFS_RUN));
    }
    if (Runs != NULL) {
        HpfsHostFree(Runs);
    }

    /* Paging I/O reads the list under the paging resource */
    (VOID)ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
    if (Fcb->Runs != NULL) {
        ExFreePool(Fcb->Runs);
    }
    Fcb->Runs = Copy;
    Fcb->RunCount = Count;
    Fcb->AllocSectors = Sectors;
    Fcb->Header.AllocationSize.QuadPart = (LONGLONG)Sectors << HPFS_SECTOR_SHIFT;
    HpfsRelease(&Fcb->PagingIoResource);

    return STATUS_SUCCESS;
}

NTSTATUS
HpfsCreateFcb (
    PHPFS_VCB Vcb,
    PHPFS_FCB ParentDcb,
    PHPFS_ENTRY Entry,
    PHPFS_FCB *NewFcb
    )
{
    PHPFS_FCB Fcb;
    BOOLEAN IsDirectory = (BOOLEAN)((Entry->Attributes & HPFS_ATTR_DIRECTORY) != 0);
    NTSTATUS Status;

    *NewFcb = NULL;

    Fcb = HpfsAllocateFcb(Vcb, (CSHORT)(IsDirectory ? HPFS_NTC_DCB : HPFS_NTC_FCB));
    if (Fcb == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = HpfsOemToUnicode(Entry->Name, Entry->NameLength, &Fcb->Name);
    if (!NT_SUCCESS(Status)) {
        HpfsDeleteFcb(Fcb);
        return Status;
    }

    Fcb->ParentDcb = ParentDcb;
    Fcb->Fnode = Entry->Fnode;
    Fcb->DirFnode = ParentDcb->Fnode;
    Fcb->Attributes = Entry->Attributes;
    Fcb->DirentFlags = (UCHAR)(Entry->Flags & ~(HPFS_DE_DOWN | HPFS_DE_END | HPFS_DE_SPECIAL));
    Fcb->AclCount = Entry->AclCount;
    Fcb->CodePage = Entry->CodePage;
    Fcb->EaSize = Entry->EaSize;
    Fcb->OemNameLength = Entry->NameLength;
    RtlCopyMemory(Fcb->OemName, Entry->Name, Entry->NameLength);
    Fcb->CreationTime = HpfsToNtTime(Entry->CreateTime);
    Fcb->LastWriteTime = HpfsToNtTime(Entry->WriteTime);
    Fcb->LastAccessTime = HpfsToNtTime(Entry->AccessTime);

    if (!IsDirectory) {
        Status = HpfsLoadRuns(Vcb, Fcb);
        if (!NT_SUCCESS(Status)) {
            HpfsDeleteFcb(Fcb);
            return Status;
        }
        if ((ULONGLONG)Entry->FileSize > (ULONGLONG)Fcb->AllocSectors << HPFS_SECTOR_SHIFT) {
            HpfsDeleteFcb(Fcb);
            return STATUS_FILE_CORRUPT_ERROR;
        }
        Fcb->Header.FileSize.QuadPart = Entry->FileSize;
        /* HPFS keeps no valid-data length: all of the file is valid */
        Fcb->Header.ValidDataLength.QuadPart = Entry->FileSize;
    }

    InsertTailList(&Vcb->FcbList, &Fcb->FcbLinks);
    ParentDcb->RefCount++;

    *NewFcb = Fcb;
    return STATUS_SUCCESS;
}

VOID
HpfsDereferenceFcb (
    PHPFS_VCB Vcb,
    PHPFS_FCB Fcb
    )
{
    PHPFS_FCB Parent;

    while (Fcb != NULL) {

        Fcb->RefCount--;

        if (Fcb->RefCount != 0 || Fcb == Vcb->RootDcb || Fcb == Vcb->VolumeFcb) {
            break;
        }

        Parent = Fcb->ParentDcb;
        HpfsDeleteFcb(Fcb);
        Fcb = Parent;
    }
}

PHPFS_FCB
HpfsFindFcbByFnode (
    PHPFS_VCB Vcb,
    ULONG Fnode
    )
{
    PLIST_ENTRY Entry;
    PHPFS_FCB Fcb;

    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {
        Fcb = CONTAINING_RECORD(Entry, HPFS_FCB, FcbLinks);
        if (Fcb->Fnode == Fnode && !(Fcb->FcbState & FCB_STATE_DELETED)) {
            return Fcb;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* CCBs                                                                */
/* ------------------------------------------------------------------ */

PHPFS_CCB
HpfsCreateCcb (
    VOID
    )
{
    PHPFS_CCB Ccb;

    Ccb = (PHPFS_CCB)ExAllocatePoolWithTag(PagedPool, sizeof(HPFS_CCB), HPFS_TAG_CCB);
    if (Ccb == NULL) {
        return NULL;
    }

    RtlZeroMemory(Ccb, sizeof(HPFS_CCB));
    Ccb->NodeTypeCode = HPFS_NTC_CCB;
    Ccb->NodeByteSize = (CSHORT)sizeof(HPFS_CCB);

    return Ccb;
}

VOID
HpfsDeleteCcb (
    PHPFS_CCB Ccb
    )
{
    if (Ccb->Pattern.Buffer != NULL) {
        ExFreePool(Ccb->Pattern.Buffer);
    }
    ExFreePool(Ccb);
}

/* ------------------------------------------------------------------ */
/* Full names                                                          */
/* ------------------------------------------------------------------ */

NTSTATUS
HpfsBuildFullName (
    PHPFS_FCB Fcb
    )
{
    PHPFS_FCB Walk;
    ULONG Length = 0;
    PWSTR Buffer;
    PWSTR Write;

    if (Fcb->FullName.Buffer != NULL) {
        return STATUS_SUCCESS;
    }

    if (Fcb->ParentDcb == NULL) {
        Length = sizeof(WCHAR);
    } else {
        for (Walk = Fcb; Walk->ParentDcb != NULL; Walk = Walk->ParentDcb) {
            Length += sizeof(WCHAR) + Walk->Name.Length;
        }
    }

    if (Length > 0xFFFE) {
        return STATUS_NAME_TOO_LONG;
    }

    Buffer = (PWSTR)ExAllocatePoolWithTag(PagedPool, Length, HPFS_TAG_NAME);
    if (Buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (Fcb->ParentDcb == NULL) {
        Buffer[0] = L'\\';
    } else {
        Write = (PWSTR)((PUCHAR)Buffer + Length);
        for (Walk = Fcb; Walk->ParentDcb != NULL; Walk = Walk->ParentDcb) {
            Write = (PWSTR)((PUCHAR)Write - Walk->Name.Length);
            RtlCopyMemory(Write, Walk->Name.Buffer, Walk->Name.Length);
            Write--;
            *Write = L'\\';
        }
    }

    Fcb->FullName.Buffer = Buffer;
    Fcb->FullName.Length = (USHORT)Length;
    Fcb->FullName.MaximumLength = (USHORT)Length;

    return STATUS_SUCCESS;
}

BOOLEAN
HpfsIsAncestor (
    PHPFS_FCB Ancestor,
    PHPFS_FCB Fcb
    )
{
    for (; Fcb != NULL; Fcb = Fcb->ParentDcb) {
        if (Fcb == Ancestor) {
            return TRUE;
        }
    }
    return FALSE;
}

VOID
HpfsForgetNames (
    PHPFS_VCB Vcb,
    PHPFS_FCB Fcb
    )
{
    PLIST_ENTRY Entry;
    PHPFS_FCB Walk;

    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {
        Walk = CONTAINING_RECORD(Entry, HPFS_FCB, FcbLinks);
        if (Walk->FullName.Buffer != NULL && HpfsIsAncestor(Fcb, Walk)) {
            ExFreePool(Walk->FullName.Buffer);
            RtlZeroMemory(&Walk->FullName, sizeof(UNICODE_STRING));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Directory entry and allocation                                      */
/* ------------------------------------------------------------------ */

VOID
HpfsFcbToEntry (
    PHPFS_FCB Fcb,
    PHPFS_ENTRY Entry
    )
{
    RtlZeroMemory(Entry, FIELD_OFFSET(HPFS_ENTRY, Name));
    Entry->Fnode = Fcb->Fnode;
    Entry->Flags = Fcb->DirentFlags;
    Entry->Attributes = Fcb->Attributes;
    Entry->CodePage = Fcb->CodePage;
    Entry->AclCount = Fcb->AclCount;
    Entry->WriteTime = HpfsFromNtTime(Fcb->LastWriteTime);
    Entry->AccessTime = HpfsFromNtTime(Fcb->LastAccessTime);
    Entry->CreateTime = HpfsFromNtTime(Fcb->CreationTime);
    Entry->FileSize = HpfsIsDcb(Fcb) ? 0 : Fcb->Header.FileSize.LowPart;
    Entry->EaSize = Fcb->EaSize;
    Entry->NameLength = Fcb->OemNameLength;
    RtlCopyMemory(Entry->Name, Fcb->OemName, Fcb->OemNameLength);
    Entry->Name[Fcb->OemNameLength] = 0;
}

/* Writes the FCB's size, times and attributes to its entry (and the fnode size) */
NTSTATUS
HpfsUpdateDirent (
    PHPFS_VCB Vcb,
    PHPFS_FCB Fcb
    )
{
    PHPFS_ENTRY Entry;
    NTSTATUS Status;

    if (Fcb->FcbState & (FCB_STATE_ROOT | FCB_STATE_DELETED)) {
        Fcb->FcbState &= ~FCB_STATE_DIRENT_DIRTY;
        return STATUS_SUCCESS;
    }
    if (Vcb->VcbState & (VCB_STATE_READ_ONLY | VCB_STATE_DISMOUNTED)) {
        return STATUS_SUCCESS;
    }

    Entry = (PHPFS_ENTRY)ExAllocatePoolWithTag(PagedPool, sizeof(HPFS_ENTRY), HPFS_TAG_BUFFER);
    if (Entry == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    HpfsFcbToEntry(Fcb, Entry);

    HpfsEngineEnter(Vcb);
    Status = STATUS_SUCCESS;
    if (HpfsIsFcb(Fcb)) {
        Status = HpfsSetFnodeSize(&Vcb->Vol, Fcb->Fnode, Fcb->Header.FileSize.LowPart);
    }
    if (NT_SUCCESS(Status)) {
        Status = HpfsDirUpdate(&Vcb->Vol, Fcb->DirFnode, Entry);
    }
    Status = HpfsEngineLeave(Vcb, Status);

    ExFreePool(Entry);

    if (NT_SUCCESS(Status)) {
        Fcb->FcbState &= ~FCB_STATE_DIRENT_DIRTY;
    }
    return Status;
}

/*
 * Makes the allocation of a file Bytes long (rounded up to sectors),
 * growing near its last run or freeing the tail. HPFS386 DASD limits are
 * charged to the directories above. Called with the file exclusive.
 */
NTSTATUS
HpfsSetAllocation (
    PHPFS_VCB Vcb,
    PHPFS_FCB Fcb,
    ULONGLONG Bytes
    )
{
    ULONG Sectors;
    ULONG Have = Fcb->AllocSectors;
    NTSTATUS Status;

    if (Bytes > HPFS_MAX_FILE_SIZE) {
        return STATUS_DISK_FULL;
    }
    Sectors = (ULONG)((Bytes + HPFS_SECTOR_SIZE - 1) >> HPFS_SECTOR_SHIFT);
    if (Sectors == Have) {
        return STATUS_SUCCESS;
    }

    HpfsEngineEnter(Vcb);
    if (Sectors > Have) {
        Status = HpfsDasdCharge(&Vcb->Vol, Fcb->DirFnode, (LONG)(Sectors - Have));
        if (NT_SUCCESS(Status)) {
            Status = HpfsGrowAllocation(&Vcb->Vol, Fcb->Fnode, Sectors);
            if (!NT_SUCCESS(Status)) {
                HpfsDasdCharge(&Vcb->Vol, Fcb->DirFnode, -(LONG)(Sectors - Have));
            }
        }
    } else {
        Status = HpfsTruncateAllocation(&Vcb->Vol, Fcb->Fnode, Sectors);
        if (NT_SUCCESS(Status)) {
            HpfsDasdCharge(&Vcb->Vol, Fcb->DirFnode, -(LONG)(Have - Sectors));
        }
    }
    Status = HpfsEngineLeave(Vcb, Status);

    if (Status == HPFS_DISK_FULL) {
        Status = STATUS_DISK_FULL;
    }
    if (NT_SUCCESS(Status) || Status == STATUS_DISK_FULL || Status == STATUS_QUOTA_EXCEEDED) {
        NTSTATUS Reload = HpfsLoadRuns(Vcb, Fcb);
        if (NT_SUCCESS(Status)) {
            Status = Reload;
        }
    }
    return Status;
}
