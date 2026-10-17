/*
 * HPFSNT - hpfstool: work with HPFS images from Linux through the shared
 * engine (the same code the driver runs).
 *
 *   hpfstool IMG format [-L label] [-s sectors] [--hpfs386] [--boot FILE]
 *   hpfstool IMG check [-q]
 *   hpfstool IMG info | ls [DIR] | lsr [DIR]
 *   hpfstool IMG mkdir DIR | rmdir DIR | rm FILE | mv OLD NEW
 *   hpfstool IMG put LOCAL PATH | get PATH LOCAL | cat PATH
 *   hpfstool IMG setea PATH NAME VALUE | getea PATH | truncate PATH SIZE
 *   hpfstool IMG copyin LOCALDIR DIR       (recursive)
 *   hpfstool IMG stress SEED OPS [MAXNAME]
 *
 * IMG is an image file; a partitioned image (MBR with an HPFS/IFS entry)
 * is used at its first such partition. IMG@N picks MBR slot N.
 */

#include "hpfsusr.h"
#include "hpfssup.h"
#include "hpfsimg.h"
#include "hpfsfmt.h"
#include "hpfschk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>

static HPFS_IMAGE Img;
static HPFS_VOL Vol;

static void
Fail(const char *What, HPFS_STATUS S)
{
    fprintf(stderr, "hpfstool: %s: status %08lX\n", What, (unsigned long)(ULONG)S);
    exit(2);
}

static int
OpenImage(const char *Spec, int ReadOnly)
{
    char Path[1024];
    const char *At = strrchr(Spec, '@');
    int Part = -2;
    UCHAR Mbr[512];

    strncpy(Path, Spec, sizeof(Path) - 1);
    Path[sizeof(Path) - 1] = 0;
    if (At && At[1] >= '0' && At[1] <= '3' && At[2] == 0) {
        Path[At - Spec] = 0;
        Part = At[1] - '0';
    }
    if (Part == -2) {
        if (ImgOpen(&Img, Path, -1, ReadOnly) != 0) return -1;
        if (ImgRead(&Img, 0, 1, Mbr) == 0 && Mbr[510] == 0x55 && Mbr[511] == 0xAA &&
            memcmp(Mbr + 54, "HPFS", 4) != 0) {
            int I;
            for (I = 0; I < 4; I++) {
                if (Mbr[446 + 16 * I + 4] == 0x07) { Part = I; break; }
            }
        }
        ImgClose(&Img);
        if (Part == -2) Part = -1;
    }
    return ImgOpen(&Img, Path, Part, ReadOnly);
}

static void
Mount(int ReadOnly)
{
    HPFS_STATUS S;
    memset(&Vol, 0, sizeof(Vol));
    Vol.HostContext = &Img;
    Vol.TotalSectors = Img.Sectors;
    S = HpfsMount(&Vol, (BOOLEAN)ReadOnly);
    if (!NT_SUCCESS(S)) Fail("mount", S);
}

/* Splits PATH into directory fnode + last component; Last empty = the root itself */
static ULONG
Resolve(const char *Path, char *Last, int WantParent)
{
    char Buf[2048], *P, *Tok, *Next;
    ULONG Dir = Vol.RootFnode;
    HPFS_ENTRY E;
    HPFS_STATUS S;

    strncpy(Buf, Path, sizeof(Buf) - 1);
    Buf[sizeof(Buf) - 1] = 0;
    for (P = Buf; *P; P++) if (*P == '\\') *P = '/';
    Last[0] = 0;
    P = Buf;
    while (*P == '/') P++;
    Tok = P;
    while (Tok && *Tok) {
        Next = strchr(Tok, '/');
        if (Next) { *Next++ = 0; while (*Next == '/') Next++; if (!*Next) Next = NULL; }
        if (!Next && WantParent) {
            strcpy(Last, Tok);
            return Dir;
        }
        S = HpfsLookup(&Vol, Dir, (const UCHAR *)Tok, (ULONG)strlen(Tok), &E);
        if (!NT_SUCCESS(S)) {
            fprintf(stderr, "hpfstool: %s: not found\n", Tok);
            exit(2);
        }
        Dir = E.Fnode;
        if (!Next) { strcpy(Last, Tok); return Dir; }
        Tok = Next;
    }
    return Dir;
}

static int
Lookup(const char *Path, ULONG *Parent, HPFS_ENTRY *E)
{
    char Last[300];
    ULONG Dir = Resolve(Path, Last, 1);
    if (!Last[0]) return -1;
    *Parent = Dir;
    return NT_SUCCESS(HpfsLookup(&Vol, Dir, (const UCHAR *)Last, (ULONG)strlen(Last), E)) ? 0 : -1;
}

static void
TimeStr(ULONG T, char *Out)
{
    time_t X = (time_t)T;
    struct tm *G = gmtime(&X);
    strftime(Out, 32, "%Y-%m-%d %H:%M:%S", G);
}

static void
ListDir(ULONG Dir, const char *Prefix, int Recurse)
{
    HPFS_ENTRY E;
    HPFS_STATUS S;
    UCHAR Prev[300];
    ULONG PrevLen = 0;
    int First = 1;
    char T[32];

    for (;;) {
        S = HpfsNextEntry(&Vol, Dir, First ? NULL : Prev, PrevLen, &E);
        if (S == HPFS_NO_MORE_ENTRIES) break;
        if (!NT_SUCCESS(S)) Fail("enumerate", S);
        First = 0;
        memcpy(Prev, E.Name, E.NameLength);
        PrevLen = E.NameLength;
        TimeStr(E.WriteTime, T);
        printf("%s %c%c%c%c%c%c %10lu %s%s%s\n", T,
               (E.Attributes & HPFS_ATTR_DIRECTORY) ? 'd' : '-',
               (E.Attributes & HPFS_ATTR_READONLY) ? 'r' : '-',
               (E.Attributes & HPFS_ATTR_HIDDEN) ? 'h' : '-',
               (E.Attributes & HPFS_ATTR_SYSTEM) ? 's' : '-',
               (E.Attributes & HPFS_ATTR_ARCHIVE) ? 'a' : '-',
               (E.Flags & HPFS_DE_EA) ? 'e' : '-',
               (unsigned long)E.FileSize, Prefix, (char *)E.Name,
               (E.Attributes & HPFS_ATTR_DIRECTORY) ? "/" : "");
        if (Recurse && (E.Attributes & HPFS_ATTR_DIRECTORY)) {
            char Sub[2048];
            snprintf(Sub, sizeof(Sub), "%s%s/", Prefix, (char *)E.Name);
            ListDir(E.Fnode, Sub, 1);
        }
    }
}

/* ------------------------------------------------------------------ */
/* File data                                                           */
/* ------------------------------------------------------------------ */

static void
WriteData(ULONG Fnode, const UCHAR *Data, ULONG Size, ULONG From)
{
    PHPFS_RUN Runs;
    ULONG Count, Sectors, I, S;
    UCHAR Sec[512];
    HPFS_STATUS St;

    St = HpfsGrowAllocation(&Vol, Fnode, (Size + 511) / 512);
    if (!NT_SUCCESS(St)) Fail("grow", St);
    St = HpfsGetRuns(&Vol, Fnode, &Runs, &Count, &Sectors);
    if (!NT_SUCCESS(St)) Fail("runs", St);
    for (I = 0; I < Count; I++) {
        for (S = 0; S < Runs[I].Length; S++) {
            ULONG Off = (Runs[I].FileSector + S) * 512;
            if (Off + 512 <= From || Off >= Size) continue;
            memset(Sec, 0, 512);
            if (Off < From) {
                ImgRead(&Img, Runs[I].DiskSector + S, 1, Sec);
            }
            {
                ULONG A = Off < From ? From : Off;
                ULONG B = Off + 512 < Size ? Off + 512 : Size;
                memcpy(Sec + (A - Off), Data + A, B - A);
            }
            ImgWrite(&Img, HpfsHotfix(&Vol, Runs[I].DiskSector + S), 1, Sec);
        }
    }
    free(Runs);
}

static UCHAR *
ReadData(ULONG Fnode, ULONG Size)
{
    PHPFS_RUN Runs;
    ULONG Count, Sectors, I, S;
    UCHAR *Out = (UCHAR *)calloc(1, ((Size + 511) & ~511UL) + 512);
    HPFS_STATUS St;

    St = HpfsGetRuns(&Vol, Fnode, &Runs, &Count, &Sectors);
    if (!NT_SUCCESS(St)) Fail("runs", St);
    for (I = 0; I < Count; I++) {
        for (S = 0; S < Runs[I].Length; S++) {
            ULONG Off = (Runs[I].FileSector + S) * 512;
            if (Off >= Size) break;
            ImgRead(&Img, HpfsHotfix(&Vol, Runs[I].DiskSector + S), 1, Out + Off);
        }
    }
    free(Runs);
    return Out;
}

static void
SetSize(ULONG Parent, HPFS_ENTRY *E, ULONG Size)
{
    HPFS_STATUS St;
    E->FileSize = Size;
    E->WriteTime = HpfsHostNow();
    St = HpfsSetFnodeSize(&Vol, E->Fnode, Size);
    if (NT_SUCCESS(St)) St = HpfsDirUpdate(&Vol, Parent, E);
    if (!NT_SUCCESS(St)) Fail("set size", St);
}

static void
PutFile(const UCHAR *Data, ULONG Size, const char *Path)
{
    char Last[300];
    ULONG Dir = Resolve(Path, Last, 1);
    HPFS_ENTRY E;
    HPFS_STATUS St;

    memset(&E, 0, sizeof(E));
    E.NameLength = (ULONG)strlen(Last);
    memcpy(E.Name, Last, E.NameLength);
    E.WriteTime = E.AccessTime = E.CreateTime = HpfsHostNow();
    E.Attributes = HPFS_ATTR_ARCHIVE;
    St = HpfsCreate(&Vol, Dir, &E, FALSE);
    if (!NT_SUCCESS(St)) Fail("create", St);
    WriteData(E.Fnode, Data, Size, 0);
    SetSize(Dir, &E, Size);
}

static UCHAR *
LoadLocal(const char *Path, ULONG *Size)
{
    FILE *F = fopen(Path, "rb");
    UCHAR *B;
    long L;
    if (!F) { perror(Path); exit(2); }
    fseek(F, 0, SEEK_END); L = ftell(F); fseek(F, 0, SEEK_SET);
    B = (UCHAR *)malloc(L + 1);
    if (fread(B, 1, L, F) != (size_t)L) { perror(Path); exit(2); }
    fclose(F);
    *Size = (ULONG)L;
    return B;
}

static void
CopyIn(const char *Local, const char *Dest)
{
    DIR *D = opendir(Local);
    struct dirent *De;
    if (!D) { perror(Local); exit(2); }
    while ((De = readdir(D)) != NULL) {
        char Src[2048], Dst[2048];
        struct stat St;
        if (!strcmp(De->d_name, ".") || !strcmp(De->d_name, "..")) continue;
        snprintf(Src, sizeof(Src), "%s/%s", Local, De->d_name);
        snprintf(Dst, sizeof(Dst), "%s/%s", Dest, De->d_name);
        if (stat(Src, &St) != 0) continue;
        if (S_ISDIR(St.st_mode)) {
            char Last[300];
            ULONG Dir = Resolve(Dst, Last, 1);
            HPFS_ENTRY E;
            HPFS_STATUS S;
            memset(&E, 0, sizeof(E));
            E.NameLength = (ULONG)strlen(Last);
            memcpy(E.Name, Last, E.NameLength);
            E.WriteTime = E.AccessTime = E.CreateTime = HpfsHostNow();
            S = HpfsCreate(&Vol, Dir, &E, TRUE);
            if (!NT_SUCCESS(S)) Fail("mkdir", S);
            CopyIn(Src, Dst);
        } else {
            ULONG Size;
            UCHAR *B = LoadLocal(Src, &Size);
            PutFile(B, Size, Dst);
            free(B);
        }
    }
    closedir(D);
}

/* ------------------------------------------------------------------ */
/* Check                                                               */
/* ------------------------------------------------------------------ */

static void
Report(void *Ctx, const char *Msg)
{
    (void)Ctx;
    printf("  %s\n", Msg);
}

static int
RunCheckEx(int Quiet, ULONG Flags)
{
    HPFS_CHECK_RESULT R;
    int E = HpfsCheckVolumeEx(&Vol, &R, Quiet ? NULL : Report, NULL, Flags);
    if (!Quiet || E) {
        printf("check: %lu errors, %lu warnings; %lu files, %lu dirs, %lu dnodes (depth %lu, %lu unbalanced), "
               "%lu anodes, %lu extents, %lu EA files, %lu free sectors, %lu free dnodes%s\n",
               (unsigned long)R.Errors, (unsigned long)R.Warnings, (unsigned long)R.Files,
               (unsigned long)R.Directories, (unsigned long)R.Dnodes, (unsigned long)R.DnodeDepth,
               (unsigned long)R.Unbalanced, (unsigned long)R.Anodes, (unsigned long)R.Extents,
               (unsigned long)R.EaFiles, (unsigned long)R.FreeSectors, (unsigned long)R.FreeDnodes,
               R.Dirty ? ", dirty" : "");
    }
    return E;
}

static int
RunCheck(int Quiet)
{
    return RunCheckEx(Quiet, 0);
}

/* ------------------------------------------------------------------ */
/* Stress test: random operations against an in-memory model          */
/* ------------------------------------------------------------------ */

typedef struct _MFILE {
    char    Path[600];
    ULONG   Parent;         /* index of the parent dir, or -1 for root */
    int     IsDir;
    int     Live;
    UCHAR  *Data;
    ULONG   Size;
    UCHAR   Ea[200];
    ULONG   EaLen;
} MFILE;

#define MAXM 20000
static MFILE *M;
static int NM;
static unsigned long long Rng;

static ULONG
Rand(ULONG N)
{
    Rng = Rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return N ? (ULONG)((Rng >> 33) % N) : 0;
}

static void
RandName(char *Out, ULONG MaxName)
{
    static const char Cs[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-. #$%&()@!{}~'";
    ULONG L = 1 + Rand(MaxName), I;
    for (I = 0; I < L; I++) Out[I] = Cs[Rand(sizeof(Cs) - 1)];
    if (Rand(4) == 0) { Out[L++] = (char)(0x80 + Rand(0x7F)); }   /* an OEM character */
    Out[L] = 0;
    while (L > 0 && (Out[L - 1] == '.' || Out[L - 1] == ' ')) Out[--L] = 0;
    if (L == 0) { strcpy(Out, "x"); }
}

static int
ModelFind(const char *Path)
{
    int I;
    for (I = 0; I < NM; I++) {
        if (M[I].Live && strlen(M[I].Path) == strlen(Path) &&
            HpfsCompareNames(&Vol, (const UCHAR *)M[I].Path, (ULONG)strlen(Path),
                             (const UCHAR *)Path, (ULONG)strlen(Path)) == 0) {
            /* compare component-wise is the same as whole-path for equal lengths */
            return I;
        }
    }
    return -1;
}

static int
PickLive(int WantDir)
{
    int Tries;
    for (Tries = 0; Tries < 200; Tries++) {
        int I = (int)Rand(NM + 1) - 1;
        if (I < 0) { if (WantDir == 1) return -1; continue; }   /* -1 = root */
        if (M[I].Live && (WantDir < 0 || M[I].IsDir == WantDir)) return I;
    }
    return WantDir == 1 ? -1 : -2;
}

static const char *
DirPath(int I)
{
    return I < 0 ? "" : M[I].Path;
}

static ULONG
DirFnodeOf(int I)
{
    char Last[300];
    if (I < 0) return Vol.RootFnode;
    return Resolve(M[I].Path, Last, 0);
}

static void
Verify(void)
{
    int I;
    for (I = 0; I < NM; I++) {
        ULONG Parent;
        HPFS_ENTRY E;
        if (!M[I].Live) continue;
        if (Lookup(M[I].Path, &Parent, &E) != 0) {
            fprintf(stderr, "verify: %s missing\n", M[I].Path);
            exit(3);
        }
        if (!M[I].IsDir) {
            UCHAR *D;
            if (E.FileSize != M[I].Size) {
                fprintf(stderr, "verify: %s size %lu, expected %lu\n", M[I].Path, (unsigned long)E.FileSize,
                        (unsigned long)M[I].Size);
                exit(3);
            }
            D = ReadData(E.Fnode, E.FileSize);
            if (memcmp(D, M[I].Data, M[I].Size) != 0) {
                fprintf(stderr, "verify: %s content differs\n", M[I].Path);
                exit(3);
            }
            free(D);
        }
        if (M[I].EaLen) {
            PUCHAR L; ULONG LL;
            if (!NT_SUCCESS(HpfsReadEas(&Vol, E.Fnode, &L, &LL)) || LL != M[I].EaLen || memcmp(L, M[I].Ea, LL)) {
                fprintf(stderr, "verify: %s EAs differ\n", M[I].Path);
                exit(3);
            }
            free(L);
        }
    }
    /* and nothing extra: count entries per directory */
}

static ULONG DirPct = 10;

static void
Stress(unsigned long Seed, unsigned long Ops, ULONG MaxName)
{
    unsigned long Op;
    ULONG Free0;
    char Name[300], Path[700], Last[300];
    HPFS_ENTRY E;
    HPFS_STATUS St;

    M = (MFILE *)calloc(MAXM, sizeof(MFILE));
    Rng = Seed * 2654435761ULL + 1;
    HpfsCountFree(&Vol, &Free0);

    for (Op = 0; Op < Ops; Op++) {
        ULONG What = Rand(100);
        int D, I;

        if (What >= 35 && What < 45 && Rand(10) >= DirPct) What = Rand(35);
        if (What < 35) {                                    /* create a file */
            D = PickLive(1);
            RandName(Name, MaxName);
            snprintf(Path, sizeof(Path), "%s/%s", DirPath(D), Name);
            if (strlen(Path) > 590 || NM >= MAXM) continue;
            I = ModelFind(Path);
            {
                ULONG Size = Rand(4) == 0 ? Rand(300000) : Rand(3000);
                UCHAR *Data = (UCHAR *)malloc(Size + 1);
                ULONG K;
                for (K = 0; K < Size; K++) Data[K] = (UCHAR)(Rand(256));
                if (I >= 0) { free(Data); continue; }
                memset(&E, 0, sizeof(E));
                E.NameLength = (ULONG)strlen(Name);
                memcpy(E.Name, Name, E.NameLength);
                E.WriteTime = E.AccessTime = E.CreateTime = 1000000 + (ULONG)Op;
                St = HpfsCreate(&Vol, DirFnodeOf(D), &E, FALSE);
                if (St == HPFS_DISK_FULL) { free(Data); continue; }
                if (!NT_SUCCESS(St)) Fail("stress create", St);
                St = HpfsGrowAllocation(&Vol, E.Fnode, (Size + 511) / 512);
                if (St == HPFS_DISK_FULL) {
                    HpfsDelete(&Vol, DirFnodeOf(D), E.Name, E.NameLength);
                    free(Data);
                    continue;
                }
                if (!NT_SUCCESS(St)) Fail("stress grow", St);
                WriteData(E.Fnode, Data, Size, 0);
                SetSize(DirFnodeOf(D), &E, Size);
                I = NM++;
                strcpy(M[I].Path, Path);
                M[I].IsDir = 0; M[I].Live = 1; M[I].Data = Data; M[I].Size = Size; M[I].EaLen = 0;
            }
        } else if (What < 45) {                             /* mkdir */
            D = PickLive(1);
            RandName(Name, MaxName);
            snprintf(Path, sizeof(Path), "%s/%s", DirPath(D), Name);
            if (strlen(Path) > 500 || NM >= MAXM || ModelFind(Path) >= 0) continue;
            memset(&E, 0, sizeof(E));
            E.NameLength = (ULONG)strlen(Name);
            memcpy(E.Name, Name, E.NameLength);
            E.WriteTime = E.AccessTime = E.CreateTime = 1000000 + (ULONG)Op;
            St = HpfsCreate(&Vol, DirFnodeOf(D), &E, TRUE);
            if (St == HPFS_DISK_FULL) continue;
            if (!NT_SUCCESS(St)) Fail("stress mkdir", St);
            I = NM++;
            strcpy(M[I].Path, Path);
            M[I].IsDir = 1; M[I].Live = 1;
        } else if (What < 65) {                             /* delete a file */
            I = PickLive(0);
            if (I < 0) continue;
            {
                ULONG Dir = Resolve(M[I].Path, Last, 1);
                St = HpfsDelete(&Vol, Dir, (const UCHAR *)Last, (ULONG)strlen(Last));
                if (!NT_SUCCESS(St)) Fail("stress delete", St);
                M[I].Live = 0;
                free(M[I].Data); M[I].Data = NULL;
            }
        } else if (What < 70) {                             /* rmdir (only if empty) */
            int J, Empty = 1;
            I = PickLive(1);
            if (I < 0) continue;
            for (J = 0; J < NM; J++) {
                if (M[J].Live && strncmp(M[J].Path, M[I].Path, strlen(M[I].Path)) == 0 &&
                    M[J].Path[strlen(M[I].Path)] == '/') { Empty = 0; break; }
            }
            {
                ULONG Dir = Resolve(M[I].Path, Last, 1);
                St = HpfsDelete(&Vol, Dir, (const UCHAR *)Last, (ULONG)strlen(Last));
                if (Empty) {
                    if (!NT_SUCCESS(St)) Fail("stress rmdir", St);
                    M[I].Live = 0;
                } else if (St != HPFS_NOT_EMPTY) {
                    Fail("stress rmdir of a full directory", St);
                }
            }
        } else if (What < 80) {                             /* append / truncate */
            ULONG Parent; ULONG NewSize;
            I = PickLive(0);
            if (I < 0) continue;
            if (Lookup(M[I].Path, &Parent, &E) != 0) Fail("stress lookup", HPFS_NOT_FOUND);
            NewSize = Rand(3) ? M[I].Size + Rand(Rand(4) ? 5000 : 200000) : Rand(M[I].Size + 1);
            if (NewSize > M[I].Size) {
                UCHAR *Data = (UCHAR *)realloc(M[I].Data, NewSize + 1);
                ULONG K;
                for (K = M[I].Size; K < NewSize; K++) Data[K] = (UCHAR)Rand(256);
                St = HpfsGrowAllocation(&Vol, E.Fnode, (NewSize + 511) / 512);
                if (St == HPFS_DISK_FULL) { M[I].Data = Data; continue; }
                if (!NT_SUCCESS(St)) Fail("stress append", St);
                WriteData(E.Fnode, Data, NewSize, M[I].Size);
                M[I].Data = Data;
            } else {
                St = HpfsTruncateAllocation(&Vol, E.Fnode, (NewSize + 511) / 512);
                if (!NT_SUCCESS(St)) Fail("stress truncate", St);
            }
            M[I].Size = NewSize;
            SetSize(Parent, &E, NewSize);
        } else if (What < 92) {                             /* rename / move */
            int J, Bad = 0;
            I = PickLive(-1);
            if (I < 0) continue;
            D = PickLive(1);
            RandName(Name, MaxName);
            snprintf(Path, sizeof(Path), "%s/%s", DirPath(D), Name);
            if (strlen(Path) > 500 || ModelFind(Path) >= 0) continue;
            if (M[I].IsDir) {
                /* not into itself */
                if (strncmp(Path, M[I].Path, strlen(M[I].Path)) == 0 && Path[strlen(M[I].Path)] == '/') continue;
                for (J = 0; J < NM; J++) {
                    if (M[J].Live && strlen(M[J].Path) + strlen(Path) > 580) Bad = 1;
                }
                if (Bad) continue;
            }
            {
                char OldLast[300];
                ULONG OldDir = Resolve(M[I].Path, OldLast, 1);
                ULONG NewDir = DirFnodeOf(D);
                St = HpfsRename(&Vol, OldDir, (const UCHAR *)OldLast, (ULONG)strlen(OldLast),
                                NewDir, (const UCHAR *)Name, (ULONG)strlen(Name));
                if (St == HPFS_DISK_FULL) continue;
                if (!NT_SUCCESS(St)) Fail("stress rename", St);
            }
            if (M[I].IsDir) {
                size_t OL = strlen(M[I].Path);
                char Old[600];
                strcpy(Old, M[I].Path);
                for (J = 0; J < NM; J++) {
                    if (M[J].Live && J != I && strncmp(M[J].Path, Old, OL) == 0 && M[J].Path[OL] == '/') {
                        char T[1300];
                        snprintf(T, sizeof(T), "%s%s", Path, M[J].Path + OL);
                        strcpy(M[J].Path, T);
                    }
                }
            }
            strcpy(M[I].Path, Path);
        } else {                                            /* EAs */
            ULONG Parent, Ext; BOOLEAN Need;
            UCHAR List[200];
            ULONG Len = 0, NameL, ValL;
            I = PickLive(-1);
            if (I < 0) continue;
            if (Lookup(M[I].Path, &Parent, &E) != 0) Fail("stress lookup", HPFS_NOT_FOUND);
            if (Rand(3)) {
                NameL = 1 + Rand(10); ValL = Rand(Rand(2) ? 40 : 150);
                if (4 + NameL + 1 + ValL > sizeof(List)) ValL = sizeof(List) - 5 - NameL;
                List[0] = (UCHAR)(Rand(5) == 0 ? HPFS_EA_NEEDEA : 0);
                List[1] = (UCHAR)NameL; List[2] = (UCHAR)ValL; List[3] = 0;
                { ULONG K; for (K = 0; K < NameL; K++) List[4 + K] = (UCHAR)('A' + Rand(26)); }
                List[4 + NameL] = 0;
                { ULONG K; for (K = 0; K < ValL; K++) List[5 + NameL + K] = (UCHAR)Rand(256); }
                Len = 5 + NameL + ValL;
            }
            St = HpfsWriteEas(&Vol, E.Fnode, List, Len, &Ext, &Need);
            if (St == HPFS_DISK_FULL) continue;
            if (!NT_SUCCESS(St)) Fail("stress EA", St);
            E.EaSize = Len;
            E.Flags = (UCHAR)((E.Flags & ~(HPFS_DE_EA | HPFS_DE_NEEDEA)) | (Len ? HPFS_DE_EA : 0) | (Need ? HPFS_DE_NEEDEA : 0));
            St = HpfsDirUpdate(&Vol, Parent, &E);
            if (!NT_SUCCESS(St)) Fail("stress EA entry", St);
            memcpy(M[I].Ea, List, Len);
            M[I].EaLen = Len;
        }

        if ((Op + 1) % 250 == 0 || Op + 1 == Ops || getenv("STRESS_CHECK")) {
            if (RunCheck(1) != 0) {
                fprintf(stderr, "stress: check failed after op %lu\n", Op + 1);
                RunCheck(0);
                exit(4);
            }
            Verify();
        }
    }

    RunCheck(0);
    if (getenv("STRESS_NOCLEAN")) return;

    /* Delete everything and expect the free space back */
    {
        int Pass, Any = 1;
        for (Pass = 0; Pass < 64 && Any; Pass++) {
            int I;
            Any = 0;
            for (I = NM - 1; I >= 0; I--) {
                if (!M[I].Live) continue;
                {
                    ULONG Dir = Resolve(M[I].Path, Last, 1);
                    St = HpfsDelete(&Vol, Dir, (const UCHAR *)Last, (ULONG)strlen(Last));
                    if (St == HPFS_NOT_EMPTY) { Any = 1; continue; }
                    if (!NT_SUCCESS(St)) { fprintf(stderr, "deleting %s\n", M[I].Path); RunCheck(0); Fail("cleanup delete", St); }
                    M[I].Live = 0;
                    if (getenv("STRESS_CHECK") && RunCheck(1)) { fprintf(stderr, "after deleting %s\n", M[I].Path); RunCheck(0); exit(4); }
                }
            }
        }
    }
    {
        ULONG Free1;
        HPFS_CHECK_RESULT R;
        if (HpfsCheckVolume(&Vol, &R, Report, NULL) != 0) { fprintf(stderr, "stress: final check failed\n"); exit(4); }
        Vol.FreeKnown = FALSE;
        { ULONG B; for (B = 0; B < Vol.BandCount; B++) Vol.BandFree[B] = HPFS_UNKNOWN; }
        HpfsCountFree(&Vol, &Free1);
        if (Free1 != Free0) {
            fprintf(stderr, "stress: free sectors %lu at start, %lu at the end\n", (unsigned long)Free0, (unsigned long)Free1);
            exit(4);
        }
        if (R.FreeDnodes != Vol.DirBandDnodes - 1 && Vol.DirBandStart > Vol.RootFnode) {
            /* root dnode lives outside the band in our layout; nothing to compare */
        }
    }
    printf("stress: %lu ops OK (seed %lu)\n", Ops, Seed);
}

/* ------------------------------------------------------------------ */

int
main(int argc, char **argv)
{
    const char *Cmd;
    HPFS_STATUS St;

    if (argc < 3) {
        fprintf(stderr, "usage: hpfstool IMG command ...\n");
        return 1;
    }
    Cmd = argv[2];

    if (!strcmp(Cmd, "format")) {
        HPFS_FORMAT_OPTIONS O;
        char Err[200];
        int I;
        UCHAR *Boot = NULL;
        memset(&O, 0, sizeof(O));
        if (OpenImage(argv[1], 0) != 0) { perror(argv[1]); return 2; }
        O.Sectors = Img.Sectors;
        O.HiddenSectors = Img.PartitionStart;
        if (O.Sectors == 0) {
            struct stat S;
            stat(argv[1], &S);
            O.Sectors = (ULONG)(S.st_size / 512);
        }
        for (I = 3; I < argc; I++) {
            if (!strcmp(argv[I], "-L") && I + 1 < argc) O.Label = argv[++I];
            else if (!strcmp(argv[I], "-s") && I + 1 < argc) O.Sectors = (ULONG)strtoul(argv[++I], 0, 0);
            else if (!strcmp(argv[I], "--hpfs386")) O.Hpfs386 = TRUE;
            else if (!strcmp(argv[I], "--boot") && I + 1 < argc) {
                ULONG Sz;
                Boot = LoadLocal(argv[++I], &Sz);
                O.BootCode = Boot;
                O.BootCodeSize = Sz;
            }
        }
        if (HpfsFormatVolume(&Img, &O, Err, sizeof(Err)) != 0) {
            fprintf(stderr, "format: %s\n", Err);
            return 2;
        }
        ImgClose(&Img);
        printf("formatted %lu sectors\n", (unsigned long)O.Sectors);
        return 0;
    }

    if (OpenImage(argv[1], 0) != 0) { perror(argv[1]); return 2; }
    Mount(0);

    if (!strcmp(Cmd, "check")) {
        int E = RunCheckEx(argc > 3 && !strcmp(argv[3], "-q"),
                           (argc > 3 && !strcmp(argv[3], "-f")) ? HPFS_CHECK_FIX : 0);
        HpfsUnmountFree(&Vol);
        ImgClose(&Img);
        return E ? 1 : 0;
    } else if (!strcmp(Cmd, "info")) {
        ULONG Free;
        HpfsCountFree(&Vol, &Free);
        printf("sectors %lu, free %lu, bands %lu, root fnode %08lX, dirband %08lX-%08lX (%lu dnodes), "
               "code page %u, label '%s', serial %08lX, version %u.%u%s\n",
               (unsigned long)Vol.TotalSectors, (unsigned long)Free, (unsigned long)Vol.BandCount,
               (unsigned long)Vol.RootFnode, (unsigned long)Vol.DirBandStart, (unsigned long)Vol.DirBandEnd,
               (unsigned long)Vol.DirBandDnodes, Vol.CodePage, (char *)Vol.Label, (unsigned long)Vol.SerialNumber,
               Vol.Version, Vol.FuncVersion, Vol.Hpfs386 ? " (HPFS386)" : "");
    } else if (!strcmp(Cmd, "ls") || !strcmp(Cmd, "lsr")) {
        char Last[300];
        ULONG Dir = argc > 3 ? Resolve(argv[3], Last, 0) : Vol.RootFnode;
        ListDir(Dir, "", Cmd[2] == 'r');
    } else if (!strcmp(Cmd, "mkdir")) {
        char Last[300];
        ULONG Dir = Resolve(argv[3], Last, 1);
        HPFS_ENTRY E;
        memset(&E, 0, sizeof(E));
        E.NameLength = (ULONG)strlen(Last);
        memcpy(E.Name, Last, E.NameLength);
        E.WriteTime = E.AccessTime = E.CreateTime = HpfsHostNow();
        St = HpfsCreate(&Vol, Dir, &E, TRUE);
        if (!NT_SUCCESS(St)) Fail("mkdir", St);
    } else if (!strcmp(Cmd, "rm") || !strcmp(Cmd, "rmdir")) {
        char Last[300];
        ULONG Dir = Resolve(argv[3], Last, 1);
        St = HpfsDelete(&Vol, Dir, (const UCHAR *)Last, (ULONG)strlen(Last));
        if (!NT_SUCCESS(St)) Fail("delete", St);
    } else if (!strcmp(Cmd, "mv")) {
        char L1[300], L2[300];
        ULONG D1 = Resolve(argv[3], L1, 1);
        ULONG D2 = Resolve(argv[4], L2, 1);
        St = HpfsRename(&Vol, D1, (const UCHAR *)L1, (ULONG)strlen(L1), D2, (const UCHAR *)L2, (ULONG)strlen(L2));
        if (!NT_SUCCESS(St)) Fail("rename", St);
    } else if (!strcmp(Cmd, "put")) {
        ULONG Size;
        UCHAR *B = LoadLocal(argv[3], &Size);
        PutFile(B, Size, argv[4]);
        free(B);
    } else if (!strcmp(Cmd, "copyin")) {
        CopyIn(argv[3], argv[4]);
    } else if (!strcmp(Cmd, "get") || !strcmp(Cmd, "cat")) {
        ULONG Parent;
        HPFS_ENTRY E;
        UCHAR *D;
        if (Lookup(argv[3], &Parent, &E) != 0) { fprintf(stderr, "not found\n"); return 2; }
        D = ReadData(E.Fnode, E.FileSize);
        if (Cmd[0] == 'c') {
            fwrite(D, 1, E.FileSize, stdout);
        } else {
            FILE *F = fopen(argv[4], "wb");
            fwrite(D, 1, E.FileSize, F);
            fclose(F);
        }
        free(D);
    } else if (!strcmp(Cmd, "truncate")) {
        ULONG Parent;
        HPFS_ENTRY E;
        ULONG Size = (ULONG)strtoul(argv[4], 0, 0);
        if (Lookup(argv[3], &Parent, &E) != 0) { fprintf(stderr, "not found\n"); return 2; }
        if (Size < E.FileSize) {
            St = HpfsTruncateAllocation(&Vol, E.Fnode, (Size + 511) / 512);
            if (!NT_SUCCESS(St)) Fail("truncate", St);
        } else {
            UCHAR *Z = (UCHAR *)calloc(1, Size + 1);
            UCHAR *Old = ReadData(E.Fnode, E.FileSize);
            memcpy(Z, Old, E.FileSize);
            WriteData(E.Fnode, Z, Size, E.FileSize);
            free(Z); free(Old);
        }
        SetSize(Parent, &E, Size);
    } else if (!strcmp(Cmd, "setea")) {
        ULONG Parent, Ext;
        BOOLEAN Need;
        HPFS_ENTRY E;
        PUCHAR Old, New;
        ULONG OldLen, NewLen, NL = (ULONG)strlen(argv[4]), VL = (ULONG)strlen(argv[5]);
        if (Lookup(argv[3], &Parent, &E) != 0) { fprintf(stderr, "not found\n"); return 2; }
        St = HpfsReadEas(&Vol, E.Fnode, &Old, &OldLen);
        if (!NT_SUCCESS(St)) Fail("read EAs", St);
        New = (PUCHAR)malloc(OldLen + 5 + NL + VL);
        if (OldLen) memcpy(New, Old, OldLen);
        NewLen = OldLen;
        New[NewLen] = 0; New[NewLen + 1] = (UCHAR)NL; New[NewLen + 2] = (UCHAR)VL; New[NewLen + 3] = (UCHAR)(VL >> 8);
        memcpy(New + NewLen + 4, argv[4], NL + 1);
        memcpy(New + NewLen + 5 + NL, argv[5], VL);
        NewLen += 5 + NL + VL;
        St = HpfsWriteEas(&Vol, E.Fnode, New, NewLen, &Ext, &Need);
        if (!NT_SUCCESS(St)) Fail("write EAs", St);
        E.EaSize = NewLen;
        E.Flags |= HPFS_DE_EA;
        St = HpfsDirUpdate(&Vol, Parent, &E);
        if (!NT_SUCCESS(St)) Fail("update entry", St);
    } else if (!strcmp(Cmd, "setacl") || !strcmp(Cmd, "getacl")) {
        /* HPFS386 ACL area: setacl PATH HEXBYTES / getacl PATH */
        ULONG Parent;
        HPFS_ENTRY E;
        if (Lookup(argv[3], &Parent, &E) != 0) { fprintf(stderr, "not found\n"); return 2; }
        if (Cmd[0] == 's') {
            UCHAR A[4096];
            ULONG N = 0;
            const char *H = argv[4];
            while (H[0] && H[1] && N < sizeof(A)) {
                unsigned V;
                sscanf(H, "%2x", &V);
                A[N++] = (UCHAR)V;
                H += 2;
            }
            St = HpfsWriteAcl(&Vol, E.Fnode, A, N);
            if (!NT_SUCCESS(St)) Fail("write ACL", St);
        } else {
            PUCHAR A;
            ULONG N, I;
            St = HpfsReadAcl(&Vol, E.Fnode, &A, &N);
            if (!NT_SUCCESS(St)) Fail("read ACL", St);
            for (I = 0; I < N; I++) printf("%02x", A[I]);
            printf("\n");
            if (A) HpfsHostFree(A);
        }
    } else if (!strcmp(Cmd, "dasd")) {
        ULONG Parent, Dir;
        HPFS_ENTRY E;
        HPFS_DASD D;
        if (!strcmp(argv[3], "/")) Dir = Vol.RootFnode;
        else if (Lookup(argv[3], &Parent, &E) != 0) { fprintf(stderr, "not found\n"); return 2; }
        else Dir = E.Fnode;
        if (argc > 4) {
            St = HpfsDasdSet(&Vol, Dir, (ULONG)strtoul(argv[4], 0, 0), 90, 10);
            if (!NT_SUCCESS(St)) Fail("set DASD limit", St);
        }
        St = HpfsDasdQuery(&Vol, Dir, &D);
        if (!NT_SUCCESS(St)) Fail("query DASD limit", St);
        printf("limit %lu, usage %lu sectors, threshold %lu%%, delta %lu%%, %s\n", (unsigned long)D.Limit,
               (unsigned long)D.Usage, (unsigned long)D.Threshold, (unsigned long)D.Delta,
               D.Active ? "enforced" : "not enforced");
    } else if (!strcmp(Cmd, "getea")) {
        ULONG Parent, Off = 0;
        HPFS_ENTRY E;
        PUCHAR L;
        ULONG Len;
        if (Lookup(argv[3], &Parent, &E) != 0) { fprintf(stderr, "not found\n"); return 2; }
        St = HpfsReadEas(&Vol, E.Fnode, &L, &Len);
        if (!NT_SUCCESS(St)) Fail("read EAs", St);
        while (Off < Len) {
            PHPFS_EA Ea = (PHPFS_EA)(L + Off);
            printf("%s%s = %u bytes: ", (Ea->Flags & HPFS_EA_NEEDEA) ? "!" : "", (char *)(L + Off + 4),
                   (unsigned)HpfsEaValueLength(Ea));
            fwrite(L + Off + 5 + Ea->NameLength, 1, HpfsEaValueLength(Ea), stdout);
            printf("\n");
            Off += HpfsEaSize(Ea);
        }
    } else if (!strcmp(Cmd, "stress")) {
        if (argc > 6) DirPct = (ULONG)strtoul(argv[6], 0, 0);
        Stress(strtoul(argv[3], 0, 0), strtoul(argv[4], 0, 0), argc > 5 ? (ULONG)strtoul(argv[5], 0, 0) : 40);
    } else {
        fprintf(stderr, "unknown command %s\n", Cmd);
        return 1;
    }
    HpfsUnmountFree(&Vol);
    ImgClose(&Img);
    return 0;
}
