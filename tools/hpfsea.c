/*
 * hpfsea - shows and changes the extended attributes of a file (on HPFS, or
 * any file system that keeps EAs), through the NT native EA calls.
 *
 *   hpfsea FILE                    lists the EAs
 *   hpfsea FILE NAME=TEXT          sets NAME to TEXT (raw bytes)
 *   hpfsea FILE /A NAME=TEXT       sets NAME to TEXT as an OS/2 EAT_ASCII value
 *   hpfsea FILE NAME=              removes NAME
 *   hpfsea FILE /TEST              exercises the EA interface (needs a scratch file)
 *
 * Runs on Windows NT 3.1 and later.
 */

#include <windows.h>

typedef LONG NTSTATUS;
#define NT_SUCCESS(s)               ((NTSTATUS)(s) >= 0)

typedef struct _HP_UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} HP_UNICODE_STRING;

typedef struct _HP_OBJECT_ATTRIBUTES {
    ULONG Length;
    HANDLE RootDirectory;
    HP_UNICODE_STRING *ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
} HP_OBJECT_ATTRIBUTES;

typedef struct _HP_IO_STATUS_BLOCK {
    NTSTATUS Status;
    ULONG_PTR Information;
} HP_IO_STATUS_BLOCK;

typedef struct _HP_FULL_EA {
    ULONG NextEntryOffset;
    UCHAR Flags;
    UCHAR EaNameLength;
    USHORT EaValueLength;
    CHAR EaName[1];
} HP_FULL_EA;

typedef struct _HP_GET_EA {
    ULONG NextEntryOffset;
    UCHAR EaNameLength;
    CHAR EaName[1];
} HP_GET_EA;

#define HP_OBJ_CASE_INSENSITIVE     0x40
#define HP_FILE_OPEN                0x01
#define HP_FILE_OVERWRITE_IF        0x05
#define HP_FILE_SYNCHRONOUS_IO_NONALERT 0x20
#define HP_FILE_NON_DIRECTORY_FILE  0x40
#define HP_FILE_NEED_EA             0x80
#define HP_STATUS_NO_EAS_ON_FILE    ((NTSTATUS)0xC0000052L)
#define HP_STATUS_NO_MORE_EAS       ((NTSTATUS)0x80000012L)
#define HP_STATUS_BUFFER_OVERFLOW   ((NTSTATUS)0x80000005L)
#define HP_STATUS_BUFFER_TOO_SMALL  ((NTSTATUS)0xC0000023L)
#define HP_STATUS_NONEXISTENT_EA    ((NTSTATUS)0xC0000051L)

__declspec(dllimport) NTSTATUS NTAPI NtCreateFile(PHANDLE, ACCESS_MASK, HP_OBJECT_ATTRIBUTES *,
    HP_IO_STATUS_BLOCK *, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
__declspec(dllimport) NTSTATUS NTAPI NtClose(HANDLE);
__declspec(dllimport) NTSTATUS NTAPI NtQueryEaFile(HANDLE, HP_IO_STATUS_BLOCK *, PVOID, ULONG, BOOLEAN,
    PVOID, ULONG, PULONG, BOOLEAN);
__declspec(dllimport) NTSTATUS NTAPI NtSetEaFile(HANDLE, HP_IO_STATUS_BLOCK *, PVOID, ULONG);

int printf(const char *, ...);
int sprintf(char *, const char *, ...);

static UCHAR Buf[70000];
static int Failures;

/* \DosDevices\C:\full\path */
static NTSTATUS
OpenTarget (
    const char *Path,
    ULONG Disposition,
    PVOID Ea,
    ULONG EaLength,
    PHANDLE Handle
    )
{
    static WCHAR Full[600];
    static WCHAR Nt[700];
    WCHAR Wide[512];
    HP_UNICODE_STRING Name;
    HP_OBJECT_ATTRIBUTES Oa;
    HP_IO_STATUS_BLOCK Iosb;
    LPWSTR FilePart;
    DWORD Len;
    int i, j;

    MultiByteToWideChar(CP_ACP, 0, Path, -1, Wide, 512);
    Len = GetFullPathNameW(Wide, 600, Full, &FilePart);
    if (Len == 0 || Len >= 600) {
        return (NTSTATUS)0xC0000033L;
    }
    lstrcpyW(Nt, L"\\DosDevices\\");
    for (i = 0, j = lstrlenW(Nt); Full[i]; i++, j++) {
        Nt[j] = Full[i];
    }
    Nt[j] = 0;

    Name.Buffer = Nt;
    Name.Length = (USHORT)(j * sizeof(WCHAR));
    Name.MaximumLength = Name.Length + sizeof(WCHAR);

    Oa.Length = sizeof(Oa);
    Oa.RootDirectory = NULL;
    Oa.ObjectName = &Name;
    Oa.Attributes = HP_OBJ_CASE_INSENSITIVE;
    Oa.SecurityDescriptor = NULL;
    Oa.SecurityQualityOfService = NULL;

    return NtCreateFile(Handle, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE | 0x10 | 0x08,
                        &Oa, &Iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, Disposition,
                        HP_FILE_SYNCHRONOUS_IO_NONALERT, Ea, EaLength);
}

static void
ShowValue (
    const UCHAR *V,
    ULONG Length
    )
{
    ULONG i;
    BOOL Text = TRUE;

    /* OS/2 EAT_ASCII: FFFD, length, text */
    if (Length >= 4 && V[0] == 0xFD && V[1] == 0xFF && (ULONG)(V[2] | (V[3] << 8)) == Length - 4) {
        printf("ASCII \"");
        for (i = 4; i < Length; i++) printf("%c", V[i]);
        printf("\"");
        return;
    }
    for (i = 0; i < Length; i++) {
        if (V[i] < 0x20 || V[i] > 0x7E) Text = FALSE;
    }
    if (Text) {
        printf("\"");
        for (i = 0; i < Length; i++) printf("%c", V[i]);
        printf("\"");
    } else {
        for (i = 0; i < Length && i < 32; i++) printf("%02X ", V[i]);
        if (Length > 32) printf("...");
    }
}

/* Prints a FILE_FULL_EA_INFORMATION list; returns the number of records */
static int
ShowList (
    const UCHAR *L
    )
{
    const HP_FULL_EA *E;
    int Count = 0;

    for (;;) {
        E = (const HP_FULL_EA *)L;
        printf("  %s%-24s %5u  ", (E->Flags & HP_FILE_NEED_EA) ? "!" : " ", E->EaName, E->EaValueLength);
        ShowValue((const UCHAR *)E->EaName + E->EaNameLength + 1, E->EaValueLength);
        printf("\n");
        Count++;
        if (E->NextEntryOffset == 0) break;
        L += E->NextEntryOffset;
    }
    return Count;
}

static ULONG
MakeEa (
    UCHAR *Out,
    const char *Name,
    const UCHAR *Value,
    ULONG ValueLength,
    BOOL Ascii,
    UCHAR Flags
    )
{
    HP_FULL_EA *E = (HP_FULL_EA *)Out;
    ULONG N = lstrlenA(Name);
    UCHAR *V;

    E->NextEntryOffset = 0;
    E->Flags = Flags;
    E->EaNameLength = (UCHAR)N;
    E->EaValueLength = (USHORT)(ValueLength + (Ascii && ValueLength ? 4 : 0));
    lstrcpyA(E->EaName, Name);
    V = (UCHAR *)E->EaName + N + 1;
    if (Ascii && ValueLength) {
        V[0] = 0xFD; V[1] = 0xFF; V[2] = (UCHAR)ValueLength; V[3] = (UCHAR)(ValueLength >> 8);
        V += 4;
    }
    CopyMemory(V, Value, ValueLength);
    return 8 + N + 1 + E->EaValueLength;
}

static void
Check (
    int Ok,
    const char *What,
    NTSTATUS Status
    )
{
    printf("%s %s (status %08lX)\n", Ok ? "ok  " : "FAIL", What, (ULONG)Status);
    if (!Ok) Failures++;
}

static int
SelfTest (
    const char *Path
    )
{
    HANDLE H;
    HP_IO_STATUS_BLOCK Iosb;
    NTSTATUS S;
    UCHAR In[600];
    ULONG Len, L2, Off;
    HP_GET_EA *G;
    UCHAR Big[3000];
    int i;

    /* Create with two EAs, one of them NEED_EA */
    Len = MakeEa(In, "first.ea", (const UCHAR *)"one", 3, FALSE, 0);
    Len = (Len + 3) & ~3UL;
    ((HP_FULL_EA *)In)->NextEntryOffset = Len;
    Len += MakeEa(In + Len, "Second", (const UCHAR *)"two two", 7, TRUE, HP_FILE_NEED_EA);
    S = OpenTarget(Path, HP_FILE_OVERWRITE_IF, In, Len, &H);
    Check(NT_SUCCESS(S), "create with an EA buffer", S);
    if (!NT_SUCCESS(S)) return 1;

    S = NtQueryEaFile(H, &Iosb, Buf, sizeof(Buf), FALSE, NULL, 0, NULL, TRUE);
    Check(NT_SUCCESS(S) && ShowList(Buf) == 2, "query all", S);
    Check(lstrcmpA(((HP_FULL_EA *)Buf)->EaName, "FIRST.EA") == 0, "names in upper case", 0);

    S = NtQueryEaFile(H, &Iosb, Buf, sizeof(Buf), TRUE, NULL, 0, NULL, TRUE);
    Check(NT_SUCCESS(S) && ((HP_FULL_EA *)Buf)->NextEntryOffset == 0, "single entry (restart)", S);
    S = NtQueryEaFile(H, &Iosb, Buf, sizeof(Buf), TRUE, NULL, 0, NULL, FALSE);
    Check(NT_SUCCESS(S) && lstrcmpA(((HP_FULL_EA *)Buf)->EaName, "SECOND") == 0 &&
          (((HP_FULL_EA *)Buf)->Flags & HP_FILE_NEED_EA), "single entry continues, need-EA flag", S);
    S = NtQueryEaFile(H, &Iosb, Buf, sizeof(Buf), TRUE, NULL, 0, NULL, FALSE);
    Check(S == HP_STATUS_NO_MORE_EAS, "then no more EAs", S);

    i = 2;
    S = NtQueryEaFile(H, &Iosb, Buf, sizeof(Buf), TRUE, NULL, 0, (PULONG)&i, FALSE);
    Check(NT_SUCCESS(S) && lstrcmpA(((HP_FULL_EA *)Buf)->EaName, "SECOND") == 0, "by index 2", S);
    i = 9;
    S = NtQueryEaFile(H, &Iosb, Buf, sizeof(Buf), TRUE, NULL, 0, (PULONG)&i, FALSE);
    Check(S == HP_STATUS_NONEXISTENT_EA, "index 9 does not exist", S);

    S = NtQueryEaFile(H, &Iosb, Buf, 12, FALSE, NULL, 0, NULL, TRUE);
    Check(S == HP_STATUS_BUFFER_TOO_SMALL, "tiny buffer", S);

    /* A list of names, one missing */
    G = (HP_GET_EA *)In;
    G->NextEntryOffset = 12;
    G->EaNameLength = 6; lstrcpyA(G->EaName, "second");
    G = (HP_GET_EA *)(In + 12);
    G->NextEntryOffset = 0;
    G->EaNameLength = 4; lstrcpyA(G->EaName, "NONE");
    S = NtQueryEaFile(H, &Iosb, Buf, sizeof(Buf), FALSE, In, 24, NULL, TRUE);
    Check(NT_SUCCESS(S) && ShowList(Buf) == 2 &&
          ((HP_FULL_EA *)(Buf + ((HP_FULL_EA *)Buf)->NextEntryOffset))->EaValueLength == 0,
          "query by name list", S);

    /* Replace one, remove one, add a large one (stored outside the fnode) */
    Len = MakeEa(In, "SECOND", (const UCHAR *)"", 0, FALSE, 0);
    S = NtSetEaFile(H, &Iosb, In, Len);
    Check(NT_SUCCESS(S), "remove SECOND", S);
    for (i = 0; i < (int)sizeof(Big); i++) Big[i] = (UCHAR)(i * 7);
    Len = MakeEa(Buf, "BIG.VALUE", Big, sizeof(Big), FALSE, 0);
    S = NtSetEaFile(H, &Iosb, Buf, Len);
    Check(NT_SUCCESS(S), "add a 3000 byte value", S);
    Len = MakeEa(In, "first.ea", (const UCHAR *)"replaced", 8, FALSE, 0);
    S = NtSetEaFile(H, &Iosb, In, Len);
    Check(NT_SUCCESS(S), "replace first.ea", S);

    S = NtQueryEaFile(H, &Iosb, Buf, sizeof(Buf), FALSE, NULL, 0, NULL, TRUE);
    Check(NT_SUCCESS(S) && ShowList(Buf) == 2, "now two EAs", S);
    for (Off = 0, L2 = 0;;) {
        HP_FULL_EA *E = (HP_FULL_EA *)(Buf + Off);
        if (lstrcmpA(E->EaName, "BIG.VALUE") == 0 && E->EaValueLength == sizeof(Big)) {
            L2 = (memcmp((UCHAR *)E->EaName + E->EaNameLength + 1, Big, sizeof(Big)) == 0);
        }
        if (E->NextEntryOffset == 0) break;
        Off += E->NextEntryOffset;
    }
    Check(L2 != 0, "large value reads back", 0);

    /* Bad name */
    Len = MakeEa(In, "A*B", (const UCHAR *)"x", 1, FALSE, 0);
    S = NtSetEaFile(H, &Iosb, In, Len);
    Check(!NT_SUCCESS(S), "illegal name refused", S);

    NtClose(H);

    /* Overwrite with no EA buffer drops them */
    S = OpenTarget(Path, HP_FILE_OVERWRITE_IF, NULL, 0, &H);
    Check(NT_SUCCESS(S), "overwrite", S);
    S = NtQueryEaFile(H, &Iosb, Buf, sizeof(Buf), FALSE, NULL, 0, NULL, TRUE);
    Check(S == HP_STATUS_NO_EAS_ON_FILE, "no EAs after overwrite", S);
    NtClose(H);

    printf("%s: %d failure(s)\n", Failures ? "FAILED" : "PASSED", Failures);
    return Failures ? 1 : 0;
}

int
main (
    int argc,
    char **argv
    )
{
    HANDLE H;
    HP_IO_STATUS_BLOCK Iosb;
    NTSTATUS S;
    BOOL Ascii = FALSE;
    int Arg = 2;
    char *Eq;
    ULONG Len;

    if (argc < 2) {
        printf("hpfsea FILE [/A] [NAME=VALUE | NAME= | /TEST]\n");
        return 2;
    }

    if (argc > 2 && lstrcmpiA(argv[2], "/TEST") == 0) {
        return SelfTest(argv[1]);
    }

    if (argc > Arg && lstrcmpiA(argv[Arg], "/A") == 0) {
        Ascii = TRUE;
        Arg++;
    }

    S = OpenTarget(argv[1], HP_FILE_OPEN, NULL, 0, &H);
    if (!NT_SUCCESS(S)) {
        printf("%s: cannot open (status %08lX)\n", argv[1], (ULONG)S);
        return 1;
    }

    if (argc > Arg) {
        Eq = argv[Arg];
        while (*Eq && *Eq != '=') Eq++;
        if (*Eq != '=') {
            printf("expected NAME=VALUE\n");
            NtClose(H);
            return 2;
        }
        *Eq++ = 0;
        Len = MakeEa(Buf, argv[Arg], (const UCHAR *)Eq, lstrlenA(Eq), Ascii, 0);
        S = NtSetEaFile(H, &Iosb, Buf, Len);
        if (!NT_SUCCESS(S)) {
            printf("cannot set the EA (status %08lX)\n", (ULONG)S);
            NtClose(H);
            return 1;
        }
    }

    S = NtQueryEaFile(H, &Iosb, Buf, sizeof(Buf), FALSE, NULL, 0, NULL, TRUE);
    if (S == HP_STATUS_NO_EAS_ON_FILE) {
        printf("%s: no extended attributes\n", argv[1]);
    } else if (!NT_SUCCESS(S)) {
        printf("%s: cannot read the EAs (status %08lX)\n", argv[1], (ULONG)S);
    } else {
        printf("%s:\n", argv[1]);
        ShowList(Buf);
    }
    NtClose(H);
    return NT_SUCCESS(S) || S == HP_STATUS_NO_EAS_ON_FILE ? 0 : 1;
}
