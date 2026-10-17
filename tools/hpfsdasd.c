/*
 * hpfsdasd - HPFS386 directory limits (DASD limits) and HPFS volume details,
 * through HPFSNT's private file system controls (src/hpfsctl.h).
 *
 *   hpfsdasd DIR                        shows the limit and usage of DIR
 *   hpfsdasd DIR /L:kb [/T:pct] [/D:pct]
 *                                       limits DIR (and everything below it)
 *                                       to kb kilobytes; /T warning threshold,
 *                                       /D warning step (percent)
 *   hpfsdasd DIR /L:0                   removes the limit
 *   hpfsdasd D: /INFO                   shows the volume's HPFS details
 *
 * The first limit set on a volume turns enforcement on (the HPFS386 "DASD
 * limits active" flag), so OS/2 HPFS386 sees and keeps the limits too.
 */

#include <windows.h>
#include <winioctl.h>
#include "hpfsctl.h"

int printf(const char *, ...);

static int
Usage(void)
{
    printf("HPFS386 directory limits.\n\n"
           "hpfsdasd dir                       show the limit and usage\n"
           "hpfsdasd dir /L:kb [/T:%%] [/D:%%]   set a limit (kilobytes)\n"
           "hpfsdasd dir /L:0                  remove the limit\n"
           "hpfsdasd drive: /INFO              show the volume's HPFS details\n");
    return 2;
}

static ULONG
Number(const char *S)
{
    ULONG V = 0;
    while (*S >= '0' && *S <= '9') V = V * 10 + (ULONG)(*S++ - '0');
    return V;
}

int
main(int argc, char **argv)
{
    char Path[MAX_PATH];
    HANDLE H;
    HPFS_DASD_INFO D;
    HPFS_VOLUME_INFO V;
    DWORD N;
    int I, Set = 0, Info = 0;
    ULONG Limit = 0, Threshold = 90, Delta = 10;

    if (argc < 2) return Usage();
    for (I = 2; I < argc; I++) {
        const char *A = argv[I];
        if (A[0] != '/' && A[0] != '-') return Usage();
        if ((A[1] == 'L' || A[1] == 'l') && A[2] == ':') { Set = 1; Limit = Number(A + 3); }
        else if ((A[1] == 'T' || A[1] == 't') && A[2] == ':') Threshold = Number(A + 3);
        else if ((A[1] == 'D' || A[1] == 'd') && A[2] == ':') Delta = Number(A + 3);
        else if (lstrcmpiA(A + 1, "INFO") == 0) Info = 1;
        else return Usage();
    }

    /* (no lstrcpynA on NT 3.1) */
    for (I = 0; argv[1][I] && I < MAX_PATH - 3; I++) Path[I] = argv[1][I];
    Path[I] = 0;
    if (Path[0] && Path[1] == ':' && Path[2] == 0) {
        Path[2] = '\\';
        Path[3] = 0;
    }

    H = CreateFileA(Path, GENERIC_READ | (Set ? GENERIC_WRITE : 0), FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (H == INVALID_HANDLE_VALUE) {
        printf("Cannot open %s (error %lu).\n", Path, (ULONG)GetLastError());
        return 1;
    }

    if (Info) {
        if (!DeviceIoControl(H, HPFS_FSCTL_QUERY_VOLUME, NULL, 0, &V, sizeof(V), &N, NULL)) {
            printf("%s is not on a volume HPFSNT serves (error %lu).\n", Path, (ULONG)GetLastError());
            CloseHandle(H);
            return 1;
        }
        printf("HPFS version %lu, function version %lu%s%s\n", V.Version, V.FuncVersion,
               V.Hpfs386 ? ", HPFS386 structures" : "", V.ReadOnly ? ", mounted read-only" : "");
        printf("%12lu kilobytes total, %lu available\n", V.TotalSectors / 2, V.FreeSectors / 2);
        printf("%12lu dnodes in the directory band\n", V.DirBandDnodes);
        printf("%12lu of %lu spare dnodes free\n", V.SpareDnodesFree, V.SpareDnodes);
        printf("%12lu hotfixed sectors\n", V.HotfixCount);
        printf("      code page %lu\n", V.CodePage);
        printf("      spareblock flags %02lX/%02lX: %s%s\n", V.SpareFlags, V.SpareFlags2,
               (V.SpareFlags & 1) ? "dirty, " : "clean, ",
               V.DasdActive ? "DASD limits enforced" : "no DASD limits");
        CloseHandle(H);
        return 0;
    }

    if (Set) {
        D.Limit = Limit * 2;
        D.Usage = 0;
        D.Threshold = Threshold;
        D.Delta = Delta;
        D.Active = 0;
        if (!DeviceIoControl(H, HPFS_FSCTL_SET_DASD, &D, sizeof(D), &D, sizeof(D), &N, NULL)) {
            printf("Cannot set the limit of %s (error %lu).\n", Path, (ULONG)GetLastError());
            CloseHandle(H);
            return 1;
        }
    } else if (!DeviceIoControl(H, HPFS_FSCTL_QUERY_DASD, NULL, 0, &D, sizeof(D), &N, NULL)) {
        printf("Cannot read the limit of %s (error %lu): not a directory on a volume HPFSNT serves?\n",
               Path, (ULONG)GetLastError());
        CloseHandle(H);
        return 1;
    }
    CloseHandle(H);

    if (D.Limit) {
        printf("%s: limit %lu KB, %lu KB used (%lu%%), warn at %lu%% and every %lu%% more%s\n", Path,
               D.Limit / 2, D.Usage / 2, D.Limit ? (ULONG)((ULONGLONG)D.Usage * 100 / D.Limit) : 0,
               D.Threshold, D.Delta, D.Active ? "" : " (not enforced on this volume)");
    } else {
        printf("%s: no limit, %lu KB used\n", Path, D.Usage / 2);
    }
    return 0;
}
