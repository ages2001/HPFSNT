/*
 * HPFSNT - hpfschk: checks (and with /F repairs) an HPFS volume or image.
 *
 *   hpfschk D: [/F] [/V]
 *   hpfschk IMAGE.IMG [/F] [/V]
 *
 * Without /F the volume is only read (it may be in use; changes made while
 * the check runs can show up as errors). /F locks the volume, repairs what
 * can be repaired safely (see chk/hpfschkc.c) and dismounts it so the
 * file system reads the repaired structures afresh. Runs on Windows NT 3.1
 * and later, where it stands in for the HPFS CHKDSK that NT 4.0 and later
 * no longer have.
 */

#include "hpfsusr.h"
#include "hpfssup.h"
#include "hpfsimg.h"
#include "hpfschk.h"
#include <stdio.h>
#include <string.h>

static int Verbose;
static ULONG Shown;

static void
Report(void *Ctx, const char *Msg)
{
    (void)Ctx;
    if (Shown++ < 400 || Verbose) {
        printf("  %s\n", Msg);
    }
}

static int
Usage(void)
{
    printf("Checks an HPFS volume.\n\n"
           "hpfschk drive: [/F] [/V]\n\n"
           "  /F   fix the errors found (locks the volume)\n"
           "  /V   list every message\n");
    return 2;
}

int
main(int argc, char **argv)
{
    HPFS_IMAGE Img;
    HPFS_VOL Vol;
    HPFS_CHECK_RESULT R;
    HPFS_STATUS St;
    const char *Target = NULL;
    int Fix = 0, IsDrive, I, Errors, Locked = 0;
    ULONG Total;

    for (I = 1; I < argc; I++) {
        const char *A = argv[I];
        if (A[0] == '/' || A[0] == '-') {
            if (!_stricmp(A + 1, "F")) Fix = 1;
            else if (!_stricmp(A + 1, "V")) Verbose = 1;
            else return Usage();
        } else if (Target == NULL) {
            Target = A;
        } else {
            return Usage();
        }
    }
    if (Target == NULL) return Usage();

    IsDrive = (Target[0] && Target[1] == ':' && Target[2] == 0);
    if (IsDrive) {
        if (ImgOpenDrive(&Img, Target[0], !Fix) != 0) {
            printf("Cannot open drive %s (error %lu).\n", Target, (unsigned long)GetLastError());
            return 3;
        }
        if (Fix) {
            if (ImgLock(&Img) != 0) {
                printf("Cannot lock drive %s: it is in use. Close the programs and files\n"
                       "using it (or check it from another system) and try again.\n", Target);
                ImgClose(&Img);
                return 3;
            }
            Locked = 1;
        }
    } else {
        UCHAR Mbr[512];
        if (ImgOpen(&Img, Target, -1, !Fix) != 0) {
            printf("Cannot open %s.\n", Target);
            return 3;
        }
        if (ImgRead(&Img, 0, 1, Mbr) == 0 && Mbr[510] == 0x55 && Mbr[511] == 0xAA &&
            memcmp(Mbr + 0x36, "HPFS", 4) != 0) {
            for (I = 0; I < 4; I++) {
                UCHAR *E = Mbr + 446 + 16 * I;
                if (E[4] == 0x07) {
                    Img.Offset = (ULONGLONG)(E[8] | (E[9] << 8) | ((ULONG)E[10] << 16) | ((ULONG)E[11] << 24)) * 512;
                    Img.Sectors = E[12] | (E[13] << 8) | ((ULONG)E[14] << 16) | ((ULONG)E[15] << 24);
                    break;
                }
            }
        }
    }

    memset(&Vol, 0, sizeof(Vol));
    Vol.HostContext = &Img;
    Vol.TotalSectors = Img.Sectors;
    St = HpfsMount(&Vol, (BOOLEAN)!Fix);
    if (!NT_SUCCESS(St)) {
        if (St == HPFS_UNRECOGNIZED) {
            printf("%s is not an HPFS volume.\n", Target);
        } else {
            printf("The HPFS structures of %s cannot be read (status %08lX).\n", Target, (unsigned long)St);
        }
        if (Locked) ImgUnlock(&Img);
        ImgClose(&Img);
        return 3;
    }

    printf("The type of the file system is HPFS%s.\n", Vol.Hpfs386 ? " (HPFS386)" : "");
    if (Vol.LabelLength) printf("Volume label: %s\n", (char *)Vol.Label);
    printf("Volume serial number: %04lX-%04lX\n", (unsigned long)(Vol.SerialNumber >> 16),
           (unsigned long)(Vol.SerialNumber & 0xFFFF));
    if (!Fix) printf("Checking (read only; /F to repair)...\n");
    else printf("Checking and repairing...\n");

    Errors = HpfsCheckVolumeEx(&Vol, &R, Report, NULL, Fix ? HPFS_CHECK_FIX : 0);
    if (Errors < 0) {
        printf("The check could not run (out of memory?).\n");
        Errors = 1;
    } else {
        Total = Vol.TotalSectors;
        printf("\n%12lu kilobytes total disk space.\n", (unsigned long)(Total / 2));
        printf("%12lu kilobytes in %lu directories.\n", (unsigned long)(R.Dnodes * 2), (unsigned long)R.Directories);
        printf("%12lu kilobytes in %lu user files.\n", (unsigned long)((R.DataSectors + R.Files) / 2), (unsigned long)R.Files);
        if (R.EaFiles) printf("%12lu files with extended attributes.\n", (unsigned long)R.EaFiles);
        if (R.Acls) printf("%12lu files with HPFS386 access control lists.\n", (unsigned long)R.Acls);
        printf("%12lu kilobytes available.\n", (unsigned long)(R.FreeSectors / 2));
        if (R.Errors == 0) {
            printf("\nNo errors found%s.\n", R.Dirty ? " (the volume is marked dirty)" : "");
        } else {
            printf("\n%lu error(s) %s.\n", (unsigned long)R.Errors,
                   Fix ? "could not be repaired" : "found; run hpfschk /F to repair");
        }
        if (R.Warnings) printf("%lu warning(s).\n", (unsigned long)R.Warnings);
    }

    HpfsUnmountFree(&Vol);
    if (Locked) {
        ImgDismount(&Img);
        ImgUnlock(&Img);
    }
    ImgClose(&Img);
    return Errors ? 1 : 0;
}
