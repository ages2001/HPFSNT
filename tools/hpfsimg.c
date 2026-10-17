/*
 * HPFSNT - user-mode host for the engine: an image file or (on Windows) a
 * volume opened with CreateFile. Mapped sector runs are read into heap
 * buffers; a run mapped twice at the same time shares one buffer, and a
 * dirty buffer is written back when its last mapping goes away.
 */

#include "hpfsusr.h"
#include "hpfssup.h"
#include "hpfsimg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#endif

typedef struct _MAPPING {
    struct _MAPPING *Next;
    ULONG       Sector;
    ULONG       Count;
    ULONG       Refs;
    BOOLEAN     Dirty;
    PUCHAR      Data;
} MAPPING;

static void
Die(const char *Msg, ULONG A, ULONG B)
{
    fprintf(stderr, "hpfsimg: %s (%lu, %lu)\n", Msg, (unsigned long)A, (unsigned long)B);
#ifdef _WIN32
    ExitProcess(3);
#else
    abort();
#endif
}

int
ImgRead(PHPFS_IMAGE Img, ULONG Sector, ULONG Count, void *Buf)
{
    ULONGLONG Off = Img->Offset + (ULONGLONG)Sector * 512;
    if (Sector + Count > Img->Sectors && Img->Sectors) return -1;
#ifdef _WIN32
    {
        LONG High = (LONG)(Off >> 32); DWORD Got;
        /* SetFilePointerEx is not in NT 3.1 */
        if (SetFilePointer(Img->Handle, (LONG)(ULONG)Off, &High, FILE_BEGIN) == 0xFFFFFFFF &&
            GetLastError() != NO_ERROR) return -1;
        if (!ReadFile(Img->Handle, Buf, Count * 512, &Got, NULL) || Got != Count * 512) return -1;
    }
#else
    if (pread(Img->Fd, Buf, (size_t)Count * 512, (off_t)Off) != (ssize_t)Count * 512) return -1;
#endif
    Img->Reads += Count;
    return 0;
}

int
ImgWrite(PHPFS_IMAGE Img, ULONG Sector, ULONG Count, const void *Buf)
{
    ULONGLONG Off = Img->Offset + (ULONGLONG)Sector * 512;
    if (Img->ReadOnly) Die("write to a read-only image", Sector, Count);
    if (Sector + Count > Img->Sectors && Img->Sectors) return -1;
#ifdef _WIN32
    {
        LONG High = (LONG)(Off >> 32); DWORD Put;
        if (SetFilePointer(Img->Handle, (LONG)(ULONG)Off, &High, FILE_BEGIN) == 0xFFFFFFFF &&
            GetLastError() != NO_ERROR) return -1;
        if (!WriteFile(Img->Handle, Buf, Count * 512, &Put, NULL) || Put != Count * 512) return -1;
    }
#else
    if (pwrite(Img->Fd, Buf, (size_t)Count * 512, (off_t)Off) != (ssize_t)Count * 512) return -1;
#endif
    Img->Writes += Count;
    return 0;
}

HPFS_STATUS
HpfsHostMap(PHPFS_VOL Vol, ULONG Sector, ULONG Count, ULONG Mode, PHPFS_BUF Buf)
{
    PHPFS_IMAGE Img = (PHPFS_IMAGE)Vol->HostContext;
    MAPPING *M;

    if (Count == 0 || Count > 256) Die("bad map size", Sector, Count);
    for (M = (MAPPING *)Img->Maps; M; M = M->Next) {
        if (M->Sector == Sector && M->Count == Count) {
            M->Refs++;
            Buf->Data = M->Data; Buf->Sector = Sector; Buf->Count = Count;
            Buf->Host = M; Buf->Dirty = FALSE;
            if (Mode == HPFS_MAP_NEW) memset(M->Data, 0, Count * 512);
            return HPFS_OK;
        }
        if (Sector < M->Sector + M->Count && M->Sector < Sector + Count) {
            Die("overlapping maps", Sector, M->Sector);
        }
    }
    M = (MAPPING *)calloc(1, sizeof(MAPPING));
    M->Data = (PUCHAR)malloc(Count * 512);
    M->Sector = Sector; M->Count = Count; M->Refs = 1;
    if (Mode == HPFS_MAP_NEW) {
        memset(M->Data, 0, Count * 512);
    } else if (ImgRead(Img, Sector, Count, M->Data) != 0) {
        free(M->Data); free(M);
        return HPFS_IO_ERROR;
    }
    M->Next = (MAPPING *)Img->Maps;
    Img->Maps = M;
    Img->Mapped++;
    Buf->Data = M->Data; Buf->Sector = Sector; Buf->Count = Count;
    Buf->Host = M; Buf->Dirty = FALSE;
    return HPFS_OK;
}

VOID
HpfsHostSetDirty(PHPFS_VOL Vol, PHPFS_BUF Buf)
{
    (void)Vol;
    ((MAPPING *)Buf->Host)->Dirty = TRUE;
    Buf->Dirty = TRUE;
}

VOID
HpfsHostUnmap(PHPFS_VOL Vol, PHPFS_BUF Buf)
{
    PHPFS_IMAGE Img = (PHPFS_IMAGE)Vol->HostContext;
    MAPPING *M = (MAPPING *)Buf->Host, **P;

    if (M == NULL) return;
    Buf->Host = NULL;
    if (--M->Refs) return;
    if (M->Dirty) {
        if (ImgWrite(Img, M->Sector, M->Count, M->Data) != 0) Die("write failed", M->Sector, M->Count);
    }
    for (P = (MAPPING **)&Img->Maps; *P; P = &(*P)->Next) {
        if (*P == M) { *P = M->Next; break; }
    }
    Img->Mapped--;
    free(M->Data);
    free(M);
}

PVOID HpfsHostAlloc(ULONG Bytes) { return malloc(Bytes ? Bytes : 1); }
VOID  HpfsHostFree(PVOID P) { free(P); }
VOID  HpfsHostFreed(PHPFS_VOL Vol, ULONG Sector, ULONG Count) { (void)Vol; (void)Sector; (void)Count; }

ULONG
HpfsHostNow(VOID)
{
#ifdef _WIN32
    SYSTEMTIME St;
    FILETIME Ft;
    ULARGE_INTEGER U;
    GetLocalTime(&St);
    SystemTimeToFileTime(&St, &Ft);
    U.LowPart = Ft.dwLowDateTime;
    U.HighPart = Ft.dwHighDateTime;
    return (ULONG)((U.QuadPart - 116444736000000000ULL) / 10000000ULL);
#else
    time_t T = time(NULL);
    struct tm *L = localtime(&T);
    /* local wall-clock time expressed as seconds since 1970 */
    return (ULONG)(T + (L ? L->tm_gmtoff : 0));
#endif
}

int
ImgOpen(PHPFS_IMAGE Img, const char *Path, int Partition, int ReadOnly)
{
    UCHAR Mbr[512];
    memset(Img, 0, sizeof(*Img));
    Img->ReadOnly = ReadOnly;
#ifdef _WIN32
    Img->Handle = CreateFileA(Path, GENERIC_READ | (ReadOnly ? 0 : GENERIC_WRITE),
                              FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (Img->Handle == INVALID_HANDLE_VALUE) return -1;
#else
    Img->Fd = open(Path, ReadOnly ? O_RDONLY : O_RDWR);
    if (Img->Fd < 0) return -1;
#endif
    if (Partition >= 0) {
        if (ImgRead(Img, 0, 1, Mbr) != 0) return -1;
        if (Mbr[510] != 0x55 || Mbr[511] != 0xAA) return -1;
        {
            UCHAR *E = Mbr + 446 + 16 * Partition;
            ULONG Start = E[8] | (E[9] << 8) | ((ULONG)E[10] << 16) | ((ULONG)E[11] << 24);
            ULONG Len = E[12] | (E[13] << 8) | ((ULONG)E[14] << 16) | ((ULONG)E[15] << 24);
            if (Len == 0) return -1;
            Img->Offset = (ULONGLONG)Start * 512;
            Img->Sectors = Len;
            Img->PartitionStart = Start;
        }
    }
    return 0;
}

#ifdef _WIN32
#include <winioctl.h>

/* A drive letter: the volume itself (\\.\X:), its size and place from the partition */
int
ImgOpenDrive(PHPFS_IMAGE Img, char Letter, int ReadOnly)
{
    char Path[8];
    PARTITION_INFORMATION Pi;
    DISK_GEOMETRY Dg;
    DWORD N;

    memset(Img, 0, sizeof(*Img));
    Img->ReadOnly = ReadOnly;
    Path[0] = '\\'; Path[1] = '\\'; Path[2] = '.'; Path[3] = '\\';
    Path[4] = Letter; Path[5] = ':'; Path[6] = 0;
    Img->Handle = CreateFileA(Path, GENERIC_READ | (ReadOnly ? 0 : GENERIC_WRITE),
                              FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (Img->Handle == INVALID_HANDLE_VALUE) return -1;
    if (DeviceIoControl(Img->Handle, IOCTL_DISK_GET_PARTITION_INFO, NULL, 0, &Pi, sizeof(Pi), &N, NULL)) {
        Img->Sectors = (ULONG)(Pi.PartitionLength.QuadPart / 512);
        Img->PartitionStart = (ULONG)(Pi.StartingOffset.QuadPart / 512);
    }
    if (DeviceIoControl(Img->Handle, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &Dg, sizeof(Dg), &N, NULL)) {
        if (Dg.BytesPerSector != 512) {
            CloseHandle(Img->Handle);
            return -2;
        }
        Img->SectorsPerTrack = Dg.SectorsPerTrack;
        Img->Heads = Dg.TracksPerCylinder;
        if (Img->Sectors == 0) {
            /* a floppy or superfloppy: no partition */
            Img->Sectors = (ULONG)(Dg.Cylinders.QuadPart * Dg.TracksPerCylinder * Dg.SectorsPerTrack);
        }
    }
    return 0;
}

int ImgLock(PHPFS_IMAGE Img)
{ DWORD N; return DeviceIoControl(Img->Handle, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &N, NULL) ? 0 : -1; }

void ImgDismount(PHPFS_IMAGE Img)
{ DWORD N; DeviceIoControl(Img->Handle, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &N, NULL); }

void ImgUnlock(PHPFS_IMAGE Img)
{ DWORD N; DeviceIoControl(Img->Handle, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &N, NULL); }

/* Partition type 07h (IFS: HPFS/NTFS) */
int ImgSetType(PHPFS_IMAGE Img, UCHAR Type)
{
    SET_PARTITION_INFORMATION Sp;
    DWORD N;
    Sp.PartitionType = Type;
    return DeviceIoControl(Img->Handle, IOCTL_DISK_SET_PARTITION_INFO, &Sp, sizeof(Sp), NULL, 0, &N, NULL) ? 0 : -1;
}
#endif

void
ImgClose(PHPFS_IMAGE Img)
{
    if (Img->Mapped) Die("maps still outstanding at close", Img->Mapped, 0);
#ifdef _WIN32
    CloseHandle(Img->Handle);
#else
    close(Img->Fd);
#endif
}

#ifdef HPFS_TRACE_CORRUPT
HPFS_STATUS
HpfsTraceCorrupt(const char *File, int Line)
{
    fprintf(stderr, "corrupt: %s:%d\n", File, Line);
    return (HPFS_STATUS)0xC0000032L;
}
#endif
