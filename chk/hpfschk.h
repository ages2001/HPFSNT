/* HPFSNT - checker core interface (chk/hpfschkc.c) */
#ifndef _HPFSCHK_H_
#define _HPFSCHK_H_

typedef struct _HPFS_CHECK_RESULT {
    ULONG   Errors;
    ULONG   Warnings;
    ULONG   Files;
    ULONG   Directories;
    ULONG   Dnodes;
    ULONG   Anodes;
    ULONG   Extents;
    ULONG   EaFiles;
    ULONG   Acls;
    ULONG   DataSectors;
    ULONG   FreeSectors;
    ULONG   FreeDnodes;
    ULONG   LostSectors;
    ULONG   DotDots;
    ULONG   DnodeDepth;
    ULONG   Unbalanced;
    ULONG   Dirty;
    ULONG   CurrentParent;      /* internal */
} HPFS_CHECK_RESULT, *PHPFS_CHECK_RESULT;

#define HPFS_CHECK_FIX          0x0001      /* repair what can be repaired safely */

/* Returns the number of errors (-1: could not run). Report gets each message. */
int HpfsCheckVolumeEx(PHPFS_VOL Vol, PHPFS_CHECK_RESULT R, void (*Report)(void *, const char *), void *Ctx,
                      ULONG Flags);
int HpfsCheckVolume(PHPFS_VOL Vol, PHPFS_CHECK_RESULT R, void (*Report)(void *, const char *), void *Ctx);

#endif
