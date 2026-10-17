/* HPFSNT - user-mode image host (see hpfsimg.c) */
#ifndef _HPFSIMG_H_
#define _HPFSIMG_H_

typedef struct _HPFS_IMAGE {
#ifdef _WIN32
    HANDLE      Handle;
#else
    int         Fd;
#endif
    ULONGLONG   Offset;             /* bytes: start of the file system */
    ULONG       Sectors;            /* 0 = unknown */
    ULONG       PartitionStart;     /* sectors (hidden sectors) */
    ULONG       SectorsPerTrack;    /* drive geometry, 0 = unknown */
    ULONG       Heads;
    int         ReadOnly;
    void       *Maps;
    ULONG       Mapped;
    ULONGLONG   Reads, Writes;
} HPFS_IMAGE, *PHPFS_IMAGE;

/* Partition: MBR slot 0..3, or -1 for a bare file system */
int  ImgOpen(PHPFS_IMAGE Img, const char *Path, int Partition, int ReadOnly);
void ImgClose(PHPFS_IMAGE Img);
int  ImgRead(PHPFS_IMAGE Img, ULONG Sector, ULONG Count, void *Buf);
int  ImgWrite(PHPFS_IMAGE Img, ULONG Sector, ULONG Count, const void *Buf);

#ifdef _WIN32
int  ImgOpenDrive(PHPFS_IMAGE Img, char Letter, int ReadOnly);
int  ImgLock(PHPFS_IMAGE Img);
void ImgDismount(PHPFS_IMAGE Img);
void ImgUnlock(PHPFS_IMAGE Img);
int  ImgSetType(PHPFS_IMAGE Img, UCHAR Type);
#endif

#endif
