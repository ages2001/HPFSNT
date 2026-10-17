/*
 * hpfsstrs - random file system workout through Win32, for testing HPFSNT
 * inside Windows NT (3.1 and later).
 *
 *   hpfsstrs DIR SEED OPS
 *
 * Creates, extends, overwrites, truncates, renames, moves and deletes files
 * (and directories) under DIR, keeping a model of what every file must
 * contain, and reads files back to compare. Prints a line per failure and
 * a summary; exit code 0 when everything matched. Run hpfschk afterwards.
 */

#include <windows.h>

int printf(const char *, ...);
int sprintf(char *, const char *, ...);

#define MAX_FILES   160
#define MAX_SIZE    (300 * 1024)
#define DIRS        6

typedef struct {
    int     Used;
    int     Dir;
    char    Name[64];
    DWORD   Size;
    DWORD   Seed;           /* content: byte i = f(Seed, i) */
} MFILE;

static MFILE Files[MAX_FILES];
static char Root[MAX_PATH];
static DWORD Rng;
static int Failures;
static UCHAR Buf[MAX_SIZE];
static UCHAR Buf2[MAX_SIZE];

static DWORD Rand(DWORD N)
{
    Rng = Rng * 1103515245 + 12345;
    return N ? (Rng >> 8) % N : 0;
}

static UCHAR Byte(DWORD Seed, DWORD I)
{
    DWORD X = Seed ^ (I * 2654435761u);
    X ^= X >> 13;
    return (UCHAR)(X * 31 + (I >> 9));
}

static void Path(char *Out, int Dir, const char *Name)
{
    if (Dir == 0) sprintf(Out, "%s\\%s", Root, Name);
    else sprintf(Out, "%s\\Sub Dir %d\\%s", Root, Dir, Name);
}

static void Fail(const char *What, const char *Name, DWORD Err)
{
    printf("FAIL %s %s (error %lu)\n", What, Name, Err);
    Failures++;
}

/* Writes the model's content of file F from Off for Len bytes */
static int WriteRange(MFILE *F, HANDLE H, DWORD Off, DWORD Len)
{
    DWORD I, Done;
    for (I = 0; I < Len; I++) Buf[I] = Byte(F->Seed, Off + I);
    if (SetFilePointer(H, Off, NULL, FILE_BEGIN) != Off) return 0;
    return WriteFile(H, Buf, Len, &Done, NULL) && Done == Len;
}

static void Verify(MFILE *F)
{
    char P[MAX_PATH];
    HANDLE H;
    DWORD Got, I;

    Path(P, F->Dir, F->Name);
    H = CreateFileA(P, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (H == INVALID_HANDLE_VALUE) { Fail("open for verify", P, GetLastError()); return; }
    if (GetFileSize(H, NULL) != F->Size) {
        printf("FAIL size %s: %lu, expected %lu\n", P, GetFileSize(H, NULL), F->Size);
        Failures++;
    } else if (!ReadFile(H, Buf2, F->Size, &Got, NULL) || Got != F->Size) {
        Fail("read", P, GetLastError());
    } else {
        for (I = 0; I < F->Size; I++) {
            if (Buf2[I] != Byte(F->Seed, I)) {
                printf("FAIL data %s at %lu\n", P, I);
                Failures++;
                break;
            }
        }
    }
    CloseHandle(H);
}

static MFILE *Pick(void)
{
    int I, Start = (int)Rand(MAX_FILES);
    for (I = 0; I < MAX_FILES; I++) {
        MFILE *F = &Files[(Start + I) % MAX_FILES];
        if (F->Used) return F;
    }
    return NULL;
}

int main(int argc, char **argv)
{
    DWORD Ops, Op, I;
    char P[MAX_PATH], Q[MAX_PATH];
    HANDLE H;
    MFILE *F;
    int D;

    if (argc < 4) {
        printf("hpfsstrs DIR SEED OPS\n");
        return 2;
    }
    lstrcpyA(Root, argv[1]);
    Rng = 0;
    for (I = 0; argv[2][I]; I++) Rng = Rng * 10 + (DWORD)(argv[2][I] - '0');
    Ops = 0;
    for (I = 0; argv[3][I]; I++) Ops = Ops * 10 + (DWORD)(argv[3][I] - '0');

    CreateDirectoryA(Root, NULL);
    for (D = 1; D < DIRS; D++) {
        sprintf(P, "%s\\Sub Dir %d", Root, D);
        CreateDirectoryA(P, NULL);
    }

    for (Op = 0; Op < Ops; Op++) {
        DWORD What = Rand(100);
        F = Pick();

        if (What < 25 || F == NULL) {
            /* create */
            for (I = 0; I < MAX_FILES && Files[I].Used; I++) ;
            if (I == MAX_FILES) continue;
            F = &Files[I];
            F->Dir = (int)Rand(DIRS);
            sprintf(F->Name, "%s file %lu.%s", Rand(2) ? "A rather long" : "f", Op,
                    Rand(2) ? "dat" : "x");
            F->Seed = Rng;
            F->Size = Rand(4) == 0 ? Rand(MAX_SIZE) : Rand(9000);
            Path(P, F->Dir, F->Name);
            H = CreateFileA(P, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
            if (H == INVALID_HANDLE_VALUE) { Fail("create", P, GetLastError()); continue; }
            if (!WriteRange(F, H, 0, F->Size)) Fail("write", P, GetLastError());
            CloseHandle(H);
            F->Used = 1;
        } else if (What < 40) {
            /* extend or overwrite part */
            DWORD Off = Rand(F->Size + 1), Len = Rand(Rand(3) ? 5000 : 80000);
            if (Off + Len > MAX_SIZE) Len = MAX_SIZE - Off;
            Path(P, F->Dir, F->Name);
            H = CreateFileA(P, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
            if (H == INVALID_HANDLE_VALUE) { Fail("open", P, GetLastError()); continue; }
            if (!WriteRange(F, H, Off, Len)) Fail("write", P, GetLastError());
            if (Off + Len > F->Size) F->Size = Off + Len;
            CloseHandle(H);
        } else if (What < 52) {
            /* truncate */
            DWORD NewSize = Rand(F->Size + 1);
            Path(P, F->Dir, F->Name);
            H = CreateFileA(P, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
            if (H == INVALID_HANDLE_VALUE) { Fail("open", P, GetLastError()); continue; }
            if (SetFilePointer(H, NewSize, NULL, FILE_BEGIN) != NewSize || !SetEndOfFile(H)) {
                Fail("truncate", P, GetLastError());
            } else {
                F->Size = NewSize;
            }
            CloseHandle(H);
        } else if (What < 64) {
            /* rename or move */
            int NewDir = (int)Rand(DIRS);
            char NewName[64];
            sprintf(NewName, "%s %lu", Rand(2) ? "Moved" : "renamed", Op);
            Path(P, F->Dir, F->Name);
            Path(Q, NewDir, NewName);
            if (!MoveFileA(P, Q)) {
                Fail("move", P, GetLastError());
            } else {
                F->Dir = NewDir;
                lstrcpyA(F->Name, NewName);
            }
        } else if (What < 80) {
            Verify(F);
        } else {
            /* delete */
            Path(P, F->Dir, F->Name);
            if (!DeleteFileA(P)) Fail("delete", P, GetLastError());
            F->Used = 0;
        }
        if ((Op + 1) % 200 == 0) printf("%lu operations, %d failures\n", Op + 1, Failures);
    }

    /* Everything once more */
    for (I = 0; I < MAX_FILES; I++) {
        if (Files[I].Used) Verify(&Files[I]);
    }

    printf("%s: %lu operations, %d failure(s)\n", Failures ? "FAILED" : "PASSED", Ops, Failures);
    return Failures ? 1 : 0;
}
