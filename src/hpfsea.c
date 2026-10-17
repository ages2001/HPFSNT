/*
 * HPFSNT - extended attributes: IRP_MJ_QUERY_EA, IRP_MJ_SET_EA and the EAs
 * given at create
 *
 * HPFS stores a file's EAs as an OS/2 FEA list (flags, name length, value
 * length, name, 0, value) in the fnode or in sectors of their own. NT sees
 * them as FILE_FULL_EA_INFORMATION records; the flags byte carries
 * FILE_NEED_EA, which has the OS/2 value. EA names are kept in upper case
 * and compared without regard to case, as OS/2 and FastFAT do.
 */

#include "hpfs.h"

#ifndef FILE_NEED_EA
#define FILE_NEED_EA                0x00000080
#endif

#define HpfsLongAlign(x)            (((x) + 3) & ~3UL)

/* Size of one record as NT returns it */
#define HpfsNtEaRecordSize(NameLength, ValueLength) \
    HpfsLongAlign(FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName) + (NameLength) + 1 + (ValueLength))

/* ------------------------------------------------------------------ */
/* The file's list                                                     */
/* ------------------------------------------------------------------ */

static NTSTATUS
HpfsReadEaList (
    PHPFS_VCB Vcb,
    ULONG Fnode,
    PUCHAR *List,
    PULONG Length
    )
{
    NTSTATUS Status;

    HpfsEngineEnter(Vcb);
    Status = HpfsReadEas(&Vcb->Vol, Fnode, List, Length);
    return HpfsEngineLeave(Vcb, Status);
}

static ULONG
HpfsNtSizeOfList (
    const UCHAR *List,
    ULONG Length
    )
{
    ULONG Off = 0, Size = 0;
    const HPFS_EA *E;

    while (Off + sizeof(HPFS_EA) <= Length) {
        E = (const HPFS_EA *)(List + Off);
        Size += HpfsNtEaRecordSize(E->NameLength, HpfsEaValueLength(E));
        Off += HpfsEaSize(E);
    }
    return Size;
}

/*
 * Bytes an NtQueryEaFile of all of the file's EAs needs: what
 * FILE_EA_INFORMATION and the directory listings report.
 */
ULONG
HpfsNtEaSizeOfFnode (
    PHPFS_VCB Vcb,
    ULONG Fnode
    )
{
    PUCHAR List = NULL;
    ULONG Length = 0;
    ULONG Size = 0;

    if (NT_SUCCESS(HpfsReadEaList(Vcb, Fnode, &List, &Length)) && List != NULL) {
        Size = HpfsNtSizeOfList(List, Length);
    }
    if (List != NULL) {
        HpfsHostFree(List);
    }
    return Size;
}

ULONG
HpfsNtEaSize (
    PHPFS_VCB Vcb,
    PHPFS_FCB Fcb
    )
{
    if (HpfsIsVfcb(Fcb) || (Fcb->FcbState & FCB_STATE_DELETED)) {
        return 0;
    }
    return HpfsNtEaSizeOfFnode(Vcb, Fcb->Fnode);
}

/* Finds Name (upper case) in an HPFS list; returns its offset or ~0 */
static ULONG
HpfsFindEa (
    PHPFS_VCB Vcb,
    const UCHAR *List,
    ULONG Length,
    const UCHAR *Name,
    ULONG NameLength
    )
{
    ULONG Off = 0, i;
    const HPFS_EA *E;
    const UCHAR *N;

    while (Off + sizeof(HPFS_EA) <= Length) {
        E = (const HPFS_EA *)(List + Off);
        if (E->NameLength == NameLength) {
            N = (const UCHAR *)(E + 1);
            for (i = 0; i < NameLength; i++) {
                if (Vcb->Vol.Upcase[N[i]] != Vcb->Vol.Upcase[Name[i]]) {
                    break;
                }
            }
            if (i == NameLength) {
                return Off;
            }
        }
        Off += HpfsEaSize(E);
    }
    return HPFS_UNKNOWN;
}

/* ------------------------------------------------------------------ */
/* Checking what the caller gives                                      */
/* ------------------------------------------------------------------ */

/*
 * A FILE_FULL_EA_INFORMATION list is well formed: every record inside the
 * buffer, names 0-terminated, links long-aligned and forward.
 */
NTSTATUS
HpfsCheckEaBuffer (
    PFILE_FULL_EA_INFORMATION Buffer,
    ULONG Length,
    PULONG ErrorOffset
    )
{
    ULONG Off = 0;
    ULONG Size;
    PFILE_FULL_EA_INFORMATION Ea;

    for (;;) {

        *ErrorOffset = Off;

        if (Off > Length || Length - Off < FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName) + 1) {
            return STATUS_EA_LIST_INCONSISTENT;
        }

        Ea = (PFILE_FULL_EA_INFORMATION)((PUCHAR)Buffer + Off);
        Size = FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName) + Ea->EaNameLength + 1 + Ea->EaValueLength;

        if (Ea->EaNameLength == 0 || Size > Length - Off || Ea->EaName[Ea->EaNameLength] != 0) {
            return STATUS_EA_LIST_INCONSISTENT;
        }

        if (Ea->NextEntryOffset == 0) {
            return STATUS_SUCCESS;
        }

        if (Ea->NextEntryOffset < Size || (Ea->NextEntryOffset & 3) != 0 ||
            Ea->NextEntryOffset > Length - Off) {
            return STATUS_EA_LIST_INCONSISTENT;
        }

        Off += Ea->NextEntryOffset;
    }
}

static BOOLEAN
HpfsIsValidEaName (
    const UCHAR *Name,
    ULONG Length
    )
{
    ULONG i;
    UCHAR C;

    if (Length == 0 || Length > 255) {
        return FALSE;
    }
    for (i = 0; i < Length; i++) {
        C = Name[i];
        if (C < 0x20 || C == '"' || C == '*' || C == '+' || C == ',' || C == '/' || C == ':' ||
            C == ';' || C == '<' || C == '=' || C == '>' || C == '?' || C == '[' || C == '\\' ||
            C == ']' || C == '|') {
            return FALSE;
        }
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Setting                                                             */
/* ------------------------------------------------------------------ */

/*
 * Applies an NT EA buffer (already checked) to a file: with Replace, the
 * buffer becomes the file's whole list; without, its records are merged
 * in (a zero-length value removes that EA). The caller holds the file
 * exclusive.
 */
NTSTATUS
HpfsSetEaBuffer (
    PHPFS_VCB Vcb,
    PHPFS_FCB Fcb,
    PFILE_FULL_EA_INFORMATION Buffer,
    ULONG Length,
    BOOLEAN Replace
    )
{
    PUCHAR Old = NULL;
    ULONG OldLength = 0;
    PUCHAR New = NULL;
    ULONG NewLength = 0;
    ULONG Room;
    ULONG Off, Pos, Size, i;
    UCHAR Name[256];
    PFILE_FULL_EA_INFORMATION Ea;
    PHPFS_EA E;
    ULONG External = 0;
    BOOLEAN NeedEa = FALSE;
    NTSTATUS Status;

    if (Buffer == NULL) {
        Length = 0;
    }

    if (Replace && Length == 0 && !(Fcb->DirentFlags & HPFS_DE_EA) && Fcb->EaSize == 0) {
        /* Nothing given, and nothing to take away (inline EAs included?) */
        Status = HpfsReadEaList(Vcb, Fcb->Fnode, &Old, &OldLength);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
        if (Old == NULL) {
            return STATUS_SUCCESS;
        }
        HpfsHostFree(Old);
        Old = NULL;
        OldLength = 0;
    } else if (!Replace) {
        Status = HpfsReadEaList(Vcb, Fcb->Fnode, &Old, &OldLength);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }

    /* The result is at most the old list plus every new record */
    Room = OldLength;
    if (Length != 0) {
        Off = 0;
        for (;;) {
            Ea = (PFILE_FULL_EA_INFORMATION)((PUCHAR)Buffer + Off);
            Room += sizeof(HPFS_EA) + Ea->EaNameLength + 1 + Ea->EaValueLength;
            if (Ea->NextEntryOffset == 0) {
                break;
            }
            Off += Ea->NextEntryOffset;
        }
    }

    New = (PUCHAR)ExAllocatePoolWithTag(PagedPool, Room ? Room : 1, HPFS_TAG_EA);
    if (New == NULL) {
        if (Old != NULL) HpfsHostFree(Old);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (OldLength != 0) {
        RtlCopyMemory(New, Old, OldLength);
        NewLength = OldLength;
    }
    if (Old != NULL) {
        HpfsHostFree(Old);
    }

    Status = STATUS_SUCCESS;
    Off = 0;
    while (Length != 0) {

        Ea = (PFILE_FULL_EA_INFORMATION)((PUCHAR)Buffer + Off);

        for (i = 0; i < Ea->EaNameLength; i++) {
            Name[i] = Vcb->Vol.Upcase[(UCHAR)Ea->EaName[i]];
        }

        if (!HpfsIsValidEaName(Name, Ea->EaNameLength)) {
            Status = STATUS_INVALID_EA_NAME;
            break;
        }

        /* An EA of the same name goes first */
        Pos = HpfsFindEa(Vcb, New, NewLength, Name, Ea->EaNameLength);
        if (Pos != HPFS_UNKNOWN) {
            Size = HpfsEaSize((PHPFS_EA)(New + Pos));
            RtlMoveMemory(New + Pos, New + Pos + Size, NewLength - Pos - Size);
            NewLength -= Size;
        }

        if (Ea->EaValueLength != 0) {
            E = (PHPFS_EA)(New + NewLength);
            E->Flags = (UCHAR)((Ea->Flags & FILE_NEED_EA) ? HPFS_EA_NEEDEA : 0);
            E->NameLength = Ea->EaNameLength;
            E->ValueLengthLo = (UCHAR)Ea->EaValueLength;
            E->ValueLengthHi = (UCHAR)(Ea->EaValueLength >> 8);
            RtlCopyMemory(E + 1, Name, Ea->EaNameLength);
            ((PUCHAR)(E + 1))[Ea->EaNameLength] = 0;
            RtlCopyMemory((PUCHAR)(E + 1) + Ea->EaNameLength + 1,
                          (PUCHAR)Ea->EaName + Ea->EaNameLength + 1, Ea->EaValueLength);
            NewLength += HpfsEaSize(E);
        }

        if (Ea->NextEntryOffset == 0) {
            break;
        }
        Off += Ea->NextEntryOffset;
    }

    if (NT_SUCCESS(Status) && NewLength > HPFS_MAX_EA_LIST) {
        Status = STATUS_EA_TOO_LARGE;
    }

    if (NT_SUCCESS(Status)) {
        HpfsEngineEnter(Vcb);
        Status = HpfsWriteEas(&Vcb->Vol, Fcb->Fnode, New, NewLength, &External, &NeedEa);
        Status = HpfsEngineLeave(Vcb, Status);
    }

    ExFreePool(New);

    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Fcb->EaSize = NewLength;
    Fcb->DirentFlags &= ~(HPFS_DE_EA | HPFS_DE_NEEDEA);
    if (NewLength != 0) {
        Fcb->DirentFlags |= HPFS_DE_EA;
    }
    if (NeedEa) {
        Fcb->DirentFlags |= HPFS_DE_NEEDEA;
    }
    Fcb->FcbState |= FCB_STATE_DIRENT_DIRTY;

    return HpfsUpdateDirent(Vcb, Fcb);
}

NTSTATUS
HpfsCommonSetEa (
    PHPFS_IRP_CONTEXT Ctx
    )
{
    PIRP Irp = Ctx->Irp;
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PHPFS_FCB Fcb = (PHPFS_FCB)FileObject->FsContext;
    PHPFS_VCB Vcb = Ctx->Vcb;
    ULONG Length = IrpSp->Parameters.SetEa.Length;
    PVOID User;
    PFILE_FULL_EA_INFORMATION Buffer = NULL;
    ULONG ErrorOffset = 0;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Fcb == NULL || HpfsIsVfcb(Fcb)) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Length == 0) {
        return STATUS_SUCCESS;
    }
    if (Length > 2 * HPFS_MAX_EA_LIST) {
        return STATUS_EA_TOO_LARGE;
    }

    User = (Irp->AssociatedIrp.SystemBuffer != NULL) ? Irp->AssociatedIrp.SystemBuffer
                                                     : HpfsMapUserBuffer(Irp);
    if (User == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    Buffer = (PFILE_FULL_EA_INFORMATION)ExAllocatePoolWithTag(PagedPool, Length, HPFS_TAG_EA);
    if (Buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = HpfsSafeCopy(Buffer, User, Length);

    if (!NT_SUCCESS(Status)) {
        ExFreePool(Buffer);
        return Status;
    }

    Status = HpfsCheckEaBuffer(Buffer, Length, &ErrorOffset);
    if (!NT_SUCCESS(Status)) {
        ExFreePool(Buffer);
        Irp->IoStatus.Information = ErrorOffset;
        return Status;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
    (VOID)ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);

    __try {

        Status = HpfsVerifyWritable(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        if (Fcb->FcbState & FCB_STATE_DELETED) {
            Status = STATUS_FILE_DELETED;
            __leave;
        }

        Status = HpfsSetEaBuffer(Vcb, Fcb, Buffer, Length, FALSE);

        if (NT_SUCCESS(Status)) {
            FileObject->Flags |= FO_FILE_MODIFIED;
            HpfsNotifyChange(Vcb, Fcb, FILE_NOTIFY_CHANGE_EA, FILE_ACTION_MODIFIED);
        }

    } __finally {

        HpfsRelease(&Fcb->Resource);
        HpfsRelease(&Vcb->Resource);
        ExFreePool(Buffer);
    }

    return Status;
}

/* ------------------------------------------------------------------ */
/* Querying                                                            */
/* ------------------------------------------------------------------ */

/* Appends one NT record at Out+*Used; FALSE if it does not fit */
static BOOLEAN
HpfsPutNtEa (
    PUCHAR Out,
    ULONG Room,
    PULONG Used,
    PULONG Last,
    BOOLEAN First,
    UCHAR Flags,
    const UCHAR *Name,
    ULONG NameLength,
    const UCHAR *Value,
    ULONG ValueLength
    )
{
    ULONG Start = HpfsLongAlign(*Used);
    ULONG Size = FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName) + NameLength + 1 + ValueLength;
    PFILE_FULL_EA_INFORMATION Ea;

    if (Start > Room || Size > Room - Start) {
        return FALSE;
    }

    if (!First) {
        ((PFILE_FULL_EA_INFORMATION)(Out + *Last))->NextEntryOffset = Start - *Last;
    }

    Ea = (PFILE_FULL_EA_INFORMATION)(Out + Start);
    Ea->NextEntryOffset = 0;
    Ea->Flags = Flags;
    Ea->EaNameLength = (UCHAR)NameLength;
    Ea->EaValueLength = (USHORT)ValueLength;
    RtlCopyMemory(Ea->EaName, Name, NameLength);
    Ea->EaName[NameLength] = 0;
    if (ValueLength != 0) {
        RtlCopyMemory((PUCHAR)Ea->EaName + NameLength + 1, Value, ValueLength);
    }

    *Last = Start;
    *Used = Start + Size;
    return TRUE;
}

/* Records of the user's FILE_GET_EA_INFORMATION list, in its order */
static NTSTATUS
HpfsQueryEaList (
    PHPFS_VCB Vcb,
    const UCHAR *List,
    ULONG ListLength,
    PFILE_GET_EA_INFORMATION Get,
    ULONG GetLength,
    BOOLEAN Single,
    PUCHAR Out,
    ULONG Room,
    PULONG Used
    )
{
    ULONG Off = 0, Pos, Last = 0, i;
    BOOLEAN First = TRUE;
    PFILE_GET_EA_INFORMATION G;
    const HPFS_EA *E;
    UCHAR Name[256];

    *Used = 0;

    for (;;) {

        if (Off > GetLength || GetLength - Off < FIELD_OFFSET(FILE_GET_EA_INFORMATION, EaName) + 1) {
            return STATUS_EA_LIST_INCONSISTENT;
        }
        G = (PFILE_GET_EA_INFORMATION)((PUCHAR)Get + Off);
        if (G->EaNameLength == 0 ||
            FIELD_OFFSET(FILE_GET_EA_INFORMATION, EaName) + G->EaNameLength + 1 > GetLength - Off) {
            return STATUS_EA_LIST_INCONSISTENT;
        }

        for (i = 0; i < G->EaNameLength; i++) {
            Name[i] = Vcb->Vol.Upcase[(UCHAR)G->EaName[i]];
        }
        if (!HpfsIsValidEaName(Name, G->EaNameLength)) {
            return STATUS_INVALID_EA_NAME;
        }

        Pos = HpfsFindEa(Vcb, List, ListLength, Name, G->EaNameLength);

        if (Pos == HPFS_UNKNOWN) {
            /* Not there: the name with an empty value */
            if (!HpfsPutNtEa(Out, Room, Used, &Last, First, 0, Name, G->EaNameLength, NULL, 0)) {
                *Used = 0;
                return STATUS_BUFFER_OVERFLOW;
            }
        } else {
            E = (const HPFS_EA *)(List + Pos);
            if (!HpfsPutNtEa(Out, Room, Used, &Last, First,
                             (UCHAR)((E->Flags & HPFS_EA_NEEDEA) ? FILE_NEED_EA : 0),
                             (const UCHAR *)(E + 1), E->NameLength,
                             (const UCHAR *)(E + 1) + E->NameLength + 1, HpfsEaValueLength(E))) {
                *Used = 0;
                return STATUS_BUFFER_OVERFLOW;
            }
        }
        First = FALSE;

        if (Single || G->NextEntryOffset == 0) {
            return STATUS_SUCCESS;
        }
        Off += G->NextEntryOffset;
    }
}

NTSTATUS
HpfsCommonQueryEa (
    PHPFS_IRP_CONTEXT Ctx
    )
{
    PIRP Irp = Ctx->Irp;
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PHPFS_FCB Fcb = (PHPFS_FCB)FileObject->FsContext;
    PHPFS_CCB Ccb = (PHPFS_CCB)FileObject->FsContext2;
    PHPFS_VCB Vcb = Ctx->Vcb;
    ULONG Length = IrpSp->Parameters.QueryEa.Length;
    PVOID GetList = IrpSp->Parameters.QueryEa.EaList;
    ULONG GetLength = IrpSp->Parameters.QueryEa.EaListLength;
    ULONG EaIndex = IrpSp->Parameters.QueryEa.EaIndex;
    BOOLEAN Single = (BOOLEAN)((IrpSp->Flags & SL_RETURN_SINGLE_ENTRY) != 0);
    PUCHAR List = NULL;
    ULONG ListLength = 0;
    PUCHAR Out = NULL;
    PUCHAR Get = NULL;
    PUCHAR User;
    ULONG Used = 0, Last = 0, Count, Index, Off, Start;
    BOOLEAN First = TRUE;
    const HPFS_EA *E;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Fcb == NULL || Ccb == NULL || HpfsIsVfcb(Fcb)) {
        return STATUS_INVALID_PARAMETER;
    }

    User = (PUCHAR)HpfsMapUserBuffer(Irp);
    if (User == NULL && Length != 0) {
        return STATUS_INVALID_PARAMETER;
    }

    Out = (PUCHAR)ExAllocatePoolWithTag(PagedPool, Length ? Length : 1, HPFS_TAG_EA);
    if (Out == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (GetList != NULL && GetLength != 0) {
        if (GetLength > 2 * HPFS_MAX_EA_LIST) {
            ExFreePool(Out);
            return STATUS_EA_LIST_INCONSISTENT;
        }
        Get = (PUCHAR)ExAllocatePoolWithTag(PagedPool, GetLength, HPFS_TAG_EA);
        if (Get == NULL) {
            ExFreePool(Out);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        Status = HpfsSafeCopy(Get, GetList, GetLength);
        if (!NT_SUCCESS(Status)) {
            ExFreePool(Get);
            ExFreePool(Out);
            return Status;
        }
    }

    (VOID)ExAcquireResourceSharedLite(&Vcb->Resource, TRUE);
    (VOID)ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);

    __try {

        Status = HpfsVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        if (Fcb->FcbState & FCB_STATE_DELETED) {
            Status = STATUS_FILE_DELETED;
            __leave;
        }

        Status = HpfsReadEaList(Vcb, Fcb->Fnode, &List, &ListLength);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        if (Get != NULL) {
            Status = HpfsQueryEaList(Vcb, List, ListLength, (PFILE_GET_EA_INFORMATION)Get, GetLength,
                                     Single, Out, Length, &Used);
            __leave;
        }

        /* Count the records */
        Count = 0;
        for (Off = 0; Off + sizeof(HPFS_EA) <= ListLength; Off += HpfsEaSize((const HPFS_EA *)(List + Off))) {
            Count++;
        }

        if (Count == 0) {
            Status = STATUS_NO_EAS_ON_FILE;
            __leave;
        }

        if (IrpSp->Flags & SL_INDEX_SPECIFIED) {
            /* 1 is the first */
            if (EaIndex == 0 || EaIndex > Count + 1) {
                Status = STATUS_NONEXISTENT_EA_ENTRY;
                __leave;
            }
            Start = EaIndex - 1;
        } else if (IrpSp->Flags & SL_RESTART_SCAN) {
            Start = 0;
        } else {
            Start = Ccb->EaIndex;
        }

        if (Start >= Count) {
            Status = STATUS_NO_MORE_EAS;
            __leave;
        }

        Index = 0;
        for (Off = 0; Off + sizeof(HPFS_EA) <= ListLength; Off += HpfsEaSize(E), Index++) {

            E = (const HPFS_EA *)(List + Off);
            if (Index < Start) {
                continue;
            }

            if (!HpfsPutNtEa(Out, Length, &Used, &Last, First,
                             (UCHAR)((E->Flags & HPFS_EA_NEEDEA) ? FILE_NEED_EA : 0),
                             (const UCHAR *)(E + 1), E->NameLength,
                             (const UCHAR *)(E + 1) + E->NameLength + 1, HpfsEaValueLength(E))) {
                Status = First ? STATUS_BUFFER_TOO_SMALL : STATUS_BUFFER_OVERFLOW;
                break;
            }

            First = FALSE;
            Ccb->EaIndex = Index + 1;

            if (Single) {
                break;
            }
        }

    } __finally {

        if (List != NULL) {
            HpfsHostFree(List);
        }
        HpfsRelease(&Fcb->Resource);
        HpfsRelease(&Vcb->Resource);
    }

    if ((NT_SUCCESS(Status) || Status == STATUS_BUFFER_OVERFLOW) && Used != 0) {
        NTSTATUS Copy = HpfsSafeCopy(User, Out, Used);
        if (!NT_SUCCESS(Copy)) {
            Status = Copy;
            Used = 0;
        }
    }

    Irp->IoStatus.Information = (NT_SUCCESS(Status) || Status == STATUS_BUFFER_OVERFLOW) ? Used : 0;

    if (Get != NULL) {
        ExFreePool(Get);
    }
    ExFreePool(Out);

    return Status;
}
