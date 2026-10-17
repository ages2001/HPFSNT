/*
 * HPFSNT - hpfsfmt: formats a volume (or an image file) as HPFS.
 *
 *   hpfsfmt D: [/V:label] [/HPFS386] [/NOBOOT] [/Y]
 *   hpfsfmt IMAGE.IMG [/V:label] [/HPFS386] [/NOBOOT]
 *
 * The volume gets the HPFSNT boot loader (sectors 0-15), which starts
 * NTLDR from the root directory, unless /NOBOOT. The partition type is set
 * to 07h. Runs on Windows NT 3.1 and later.
 */

#include "hpfsusr.h"
#include "hpfssup.h"
#include "hpfsimg.h"
#include "hpfsfmt.h"
#include "hpfsboot.h"
#include <stdio.h>
#include <string.h>

static int
Usage(void)
{
    printf("Formats a volume as HPFS.\n\n"
           "hpfsfmt drive: [/V:label] [/HPFS386] [/NOBOOT] [/Y]\n\n"
           "  /V:label   volume label (up to 11 characters)\n"
           "  /HPFS386   mark the volume as HPFS386 (function version 3)\n"
           "  /NOBOOT    do not write the boot loader\n"
           "  /Y         do not ask for confirmation\n");
    return 2;
}

int
main(int argc, char **argv)
{
    HPFS_IMAGE Img;
    HPFS_FORMAT_OPTIONS O;
    const char *Target = NULL;
    const char *Label = NULL;
    char Err[200];
    char Answer[8];
    int Yes = 0, NoBoot = 0, IsDrive, I, Locked = 0;
    DWORD N;

    memset(&O, 0, sizeof(O));
    for (I = 1; I < argc; I++) {
        const char *A = argv[I];
        if (A[0] == '/' || A[0] == '-') {
            if ((A[1] == 'V' || A[1] == 'v') && A[2] == ':') Label = A + 3;
            else if (!_stricmp(A + 1, "HPFS386")) O.Hpfs386 = TRUE;
            else if (!_stricmp(A + 1, "NOBOOT")) NoBoot = 1;
            else if (!_stricmp(A + 1, "Y")) Yes = 1;
            else return Usage();
        } else if (Target == NULL) {
            Target = A;
        } else {
            return Usage();
        }
    }
    if (Target == NULL) return Usage();
    if (Label && strlen(Label) > 11) {
        printf("The label is longer than 11 characters.\n");
        return 2;
    }

    IsDrive = (Target[0] && Target[1] == ':' && Target[2] == 0);
    if (IsDrive) {
        if (ImgOpenDrive(&Img, Target[0], 0) != 0) {
            printf("Cannot open drive %s (error %lu).\n", Target, (unsigned long)GetLastError());
            return 1;
        }
        if (Img.Sectors == 0) {
            printf("Cannot tell the size of drive %s.\n", Target);
            return 1;
        }
        if (!Yes) {
            printf("WARNING: ALL DATA ON DRIVE %c: WILL BE LOST!\nProceed with format (Y/N)? ", Target[0]);
            if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), Answer, sizeof(Answer), &N, NULL) || N == 0 ||
                (Answer[0] != 'y' && Answer[0] != 'Y')) {
                printf("\nFormat cancelled.\n");
                return 1;
            }
        }
        if (ImgLock(&Img) != 0) {
            printf("Cannot lock drive %s: it is in use (open files, a running program,\n"
                   "the system or paging file). Close them and try again.\n", Target);
            ImgClose(&Img);
            return 1;
        }
        Locked = 1;
        /* Whatever was mounted lets go of the volume before it is rewritten */
        ImgDismount(&Img);
    } else {
        if (ImgOpen(&Img, Target, -1, 0) != 0) {
            printf("Cannot open %s.\n", Target);
            return 1;
        }
        /* A partitioned image: the first IFS (07h) partition */
        {
            UCHAR Mbr[512];
            if (ImgRead(&Img, 0, 1, Mbr) == 0 && Mbr[510] == 0x55 && Mbr[511] == 0xAA &&
                Mbr[0x36] != 'H' && Mbr[0x0B] == 0) {
                for (I = 0; I < 4; I++) {
                    UCHAR *E = Mbr + 446 + 16 * I;
                    if (E[4] == 0x07) {
                        Img.Offset = (ULONGLONG)(E[8] | (E[9] << 8) | ((ULONG)E[10] << 16) | ((ULONG)E[11] << 24)) * 512;
                        Img.PartitionStart = (ULONG)(Img.Offset / 512);
                        Img.Sectors = E[12] | (E[13] << 8) | ((ULONG)E[14] << 16) | ((ULONG)E[15] << 24);
                        break;
                    }
                }
            }
        }
        if (Img.Sectors == 0) {
            DWORD High = 0, Low = GetFileSize(Img.Handle, &High);
            Img.Sectors = (ULONG)((((ULONGLONG)High << 32) | Low) / 512);
        }
    }

    if (Img.Sectors > HPFS_MAX_SECTORS) {
        printf("The volume is larger than HPFS can hold (64 GB); %lu MB will be used.\n",
               (unsigned long)(HPFS_MAX_SECTORS / 2048));
        Img.Sectors = HPFS_MAX_SECTORS;
    }

    O.Sectors = Img.Sectors;
    O.HiddenSectors = Img.PartitionStart;
    O.SectorsPerTrack = Img.SectorsPerTrack;
    O.Heads = Img.Heads;
    O.Label = Label;
    if (!NoBoot) {
        O.BootCode = HpfsBootLoader;
        O.BootCodeSize = sizeof(HpfsBootLoader);
    }

    printf("Formatting %lu MB as HPFS%s...\n", (unsigned long)(O.Sectors / 2048), O.Hpfs386 ? "386" : "");
    if (HpfsFormatVolume(&Img, &O, Err, sizeof(Err)) != 0) {
        printf("Format failed: %s\n", Err);
        if (Locked) ImgUnlock(&Img);
        ImgClose(&Img);
        return 1;
    }

    if (IsDrive) {
        if (ImgSetType(&Img, 0x07) != 0) {
            printf("Note: the partition type could not be set to 07h (error %lu).\n",
                   (unsigned long)GetLastError());
        }
        ImgDismount(&Img);
        ImgUnlock(&Img);
    }
    ImgClose(&Img);

    printf("Format complete.\n%10lu kilobytes total disk space.\n", (unsigned long)(O.Sectors / 2));
    return 0;
}
