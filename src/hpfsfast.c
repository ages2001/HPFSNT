/*
 * HPFSNT - fast I/O and cache manager callbacks
 */

#include "hpfs.h"

UCHAR
HpfsIsFastIoPossible (
    PHPFS_FCB Fcb
    )
{
    if (Fcb->Vcb->VcbState & VCB_STATE_DISMOUNTED) {
        return FastIoIsNotPossible;
    }

    if (FsRtlAreThereCurrentFileLocks(&Fcb->FileLock)) {
        return FastIoIsQuestionable;
    }

    return FastIoIsPossible;
}

static BOOLEAN
HpfsFastIoUsable (
    PHPFS_FCB Fcb
    )
{
    if (Fcb == NULL || (!HpfsIsFcb(Fcb) && !HpfsIsDcb(Fcb))) {
        return FALSE;
    }

    if ((Fcb->Vcb->VcbState & VCB_STATE_DISMOUNTED) ||
        (Fcb->Vcb->Vpb->RealDevice->Flags & DO_VERIFY_VOLUME)) {

        return FALSE;
    }

    return TRUE;
}

static BOOLEAN
NTAPI
HpfsFastIoCheckIfPossible (
    PFILE_OBJECT FileObject,
    PLARGE_INTEGER FileOffset,
    ULONG Length,
    BOOLEAN Wait,
    ULONG LockKey,
    BOOLEAN CheckForReadOperation,
    PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject
    )
{
    PHPFS_FCB Fcb = (PHPFS_FCB)FileObject->FsContext;
    LARGE_INTEGER LargeLength;

    UNREFERENCED_PARAMETER(Wait);
    UNREFERENCED_PARAMETER(IoStatus);
    UNREFERENCED_PARAMETER(DeviceObject);

    if (!HpfsFastIoUsable(Fcb) || !HpfsIsFcb(Fcb)) {
        return FALSE;
    }

    LargeLength.QuadPart = Length;

    if (CheckForReadOperation) {
        return FsRtlFastCheckLockForRead(&Fcb->FileLock, FileOffset, &LargeLength, LockKey,
                                         FileObject, IoGetCurrentProcess());
    }

    if (Fcb->Vcb->VcbState & VCB_STATE_READ_ONLY) {
        return FALSE;
    }

    return FsRtlFastCheckLockForWrite(&Fcb->FileLock, FileOffset, &LargeLength, LockKey,
                                      FileObject, IoGetCurrentProcess());
}

static BOOLEAN
NTAPI
HpfsFastQueryBasicInfo (
    PFILE_OBJECT FileObject,
    BOOLEAN Wait,
    PFILE_BASIC_INFORMATION Buffer,
    PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject
    )
{
    PHPFS_FCB Fcb = (PHPFS_FCB)FileObject->FsContext;
    FILE_BASIC_INFORMATION Info;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!HpfsFastIoUsable(Fcb)) {
        return FALSE;
    }

    FsRtlEnterFileSystem();

    if (!ExAcquireResourceSharedLite(&Fcb->Resource, Wait)) {
        FsRtlExitFileSystem();
        return FALSE;
    }

    HpfsFillBasicInfo(Fcb, &Info);

    HpfsRelease(&Fcb->Resource);
    FsRtlExitFileSystem();

    *Buffer = Info;
    IoStatus->Status = STATUS_SUCCESS;
    IoStatus->Information = sizeof(FILE_BASIC_INFORMATION);

    return TRUE;
}

static BOOLEAN
NTAPI
HpfsFastQueryStandardInfo (
    PFILE_OBJECT FileObject,
    BOOLEAN Wait,
    PFILE_STANDARD_INFORMATION Buffer,
    PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject
    )
{
    PHPFS_FCB Fcb = (PHPFS_FCB)FileObject->FsContext;
    FILE_STANDARD_INFORMATION Info;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!HpfsFastIoUsable(Fcb)) {
        return FALSE;
    }

    FsRtlEnterFileSystem();

    if (!ExAcquireResourceSharedLite(&Fcb->Resource, Wait)) {
        FsRtlExitFileSystem();
        return FALSE;
    }

    HpfsFillStandardInfo(Fcb, &Info);

    HpfsRelease(&Fcb->Resource);
    FsRtlExitFileSystem();

    *Buffer = Info;
    IoStatus->Status = STATUS_SUCCESS;
    IoStatus->Information = sizeof(FILE_STANDARD_INFORMATION);

    return TRUE;
}

static BOOLEAN
NTAPI
HpfsFastQueryNetworkOpenInfo (
    PFILE_OBJECT FileObject,
    BOOLEAN Wait,
    PFILE_NETWORK_OPEN_INFORMATION Buffer,
    PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject
    )
{
    PHPFS_FCB Fcb = (PHPFS_FCB)FileObject->FsContext;
    FILE_NETWORK_OPEN_INFORMATION Info;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!HpfsFastIoUsable(Fcb)) {
        return FALSE;
    }

    FsRtlEnterFileSystem();

    if (!ExAcquireResourceSharedLite(&Fcb->Resource, Wait)) {
        FsRtlExitFileSystem();
        return FALSE;
    }

    HpfsFillNetworkOpenInfo(Fcb, &Info);

    HpfsRelease(&Fcb->Resource);
    FsRtlExitFileSystem();

    *Buffer = Info;
    IoStatus->Status = STATUS_SUCCESS;
    IoStatus->Information = sizeof(FILE_NETWORK_OPEN_INFORMATION);

    return TRUE;
}

VOID
HpfsInitializeFastIo (
    PFAST_IO_DISPATCH FastIo
    )
{
    RtlZeroMemory(FastIo, sizeof(FAST_IO_DISPATCH));

    FastIo->SizeOfFastIoDispatch = sizeof(FAST_IO_DISPATCH);
    FastIo->FastIoCheckIfPossible = HpfsFastIoCheckIfPossible;
    FastIo->FastIoRead = FsRtlCopyRead;
    FastIo->FastIoWrite = FsRtlCopyWrite;
    FastIo->FastIoQueryBasicInfo = HpfsFastQueryBasicInfo;
    FastIo->FastIoQueryStandardInfo = HpfsFastQueryStandardInfo;
    FastIo->FastIoQueryNetworkOpenInfo = HpfsFastQueryNetworkOpenInfo;
}

/* ------------------------------------------------------------------ */
/* Cache manager callbacks                                             */
/* ------------------------------------------------------------------ */

static BOOLEAN
NTAPI
HpfsAcquireForLazyWrite (
    PVOID Context,
    BOOLEAN Wait
    )
{
    PHPFS_FCB Fcb = (PHPFS_FCB)Context;

    FsRtlEnterFileSystem();

    if (!ExAcquireResourceSharedLite(&Fcb->PagingIoResource, Wait)) {
        FsRtlExitFileSystem();
        return FALSE;
    }

    /* Paging writes from this thread must not move VDL */
    Fcb->LazyWriteThread = KeGetCurrentThread();

    if (IoGetTopLevelIrp() == NULL) {
        IoSetTopLevelIrp((PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);
    }

    return TRUE;
}

static VOID
NTAPI
HpfsReleaseFromLazyWrite (
    PVOID Context
    )
{
    PHPFS_FCB Fcb = (PHPFS_FCB)Context;

    if (IoGetTopLevelIrp() == (PIRP)FSRTL_CACHE_TOP_LEVEL_IRP) {
        IoSetTopLevelIrp(NULL);
    }

    Fcb->LazyWriteThread = NULL;

    HpfsRelease(&Fcb->PagingIoResource);
    FsRtlExitFileSystem();
}

static BOOLEAN
NTAPI
HpfsAcquireForReadAhead (
    PVOID Context,
    BOOLEAN Wait
    )
{
    PHPFS_FCB Fcb = (PHPFS_FCB)Context;

    FsRtlEnterFileSystem();

    if (!ExAcquireResourceSharedLite(&Fcb->Resource, Wait)) {
        FsRtlExitFileSystem();
        return FALSE;
    }

    if (IoGetTopLevelIrp() == NULL) {
        IoSetTopLevelIrp((PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);
    }

    return TRUE;
}

static VOID
NTAPI
HpfsReleaseFromReadAhead (
    PVOID Context
    )
{
    PHPFS_FCB Fcb = (PHPFS_FCB)Context;

    if (IoGetTopLevelIrp() == (PIRP)FSRTL_CACHE_TOP_LEVEL_IRP) {
        IoSetTopLevelIrp(NULL);
    }

    HpfsRelease(&Fcb->Resource);
    FsRtlExitFileSystem();
}

VOID
HpfsInitializeCacheCallbacks (
    VOID
    )
{
    HpfsData.CacheManagerCallbacks.AcquireForLazyWrite = HpfsAcquireForLazyWrite;
    HpfsData.CacheManagerCallbacks.ReleaseFromLazyWrite = HpfsReleaseFromLazyWrite;
    HpfsData.CacheManagerCallbacks.AcquireForReadAhead = HpfsAcquireForReadAhead;
    HpfsData.CacheManagerCallbacks.ReleaseFromReadAhead = HpfsReleaseFromReadAhead;

}
