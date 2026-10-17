/*
 * HPFSNT - HPFS / HPFS386 file system driver for Windows NT 3.1, 3.5, 3.51,
 * 4.0, 2000 and XP; stands in for pinball.sys.
 *
 *   HPFS_NT4   Windows NT 3.1 - 4.0 builds
 *   HPFS_CROSS GCC cross build (implies HPFS_NT31 and HPFS_NT4): one binary
 *              for NT 3.1 - 4.0, also loaded by 2000 / XP
 *
 * The on-disk work is done by the shared engine (hpfssup.c, hpfsalc.c,
 * hpfsdnd.c, hpfsfil.c), which reaches the disk through the metadata cache
 * in hpfsmeta.c. File data is cached by the Cache Manager in each file's
 * own section and goes to the disk non-cached through the file's run list.
 */

#ifndef _HPFS_H_
#define _HPFS_H_

#ifndef HPFS_KERNEL
#define HPFS_KERNEL
#endif

#ifdef HPFS_CROSS
#ifndef HPFS_NT31
#define HPFS_NT31
#endif
#ifndef HPFS_NT4
#define HPFS_NT4
#endif
#endif /* HPFS_CROSS */

/*
 * GCC has no __try/__finally: the handlers' cleanup blocks become plain code
 * reached by goto (__leave). Exceptions are caught with HpfsTryCall.
 */
#ifdef __GNUC__
#define __try
#define __finally               __hpfs_leave: __attribute__((unused));
#define __except(x)             if (0)
#define __leave                 goto __hpfs_leave
#ifndef AbnormalTermination
#define AbnormalTermination()   0
#endif
#ifndef GetExceptionInformation
#define GetExceptionInformation() ((PEXCEPTION_POINTERS)0)
#endif
#ifndef GetExceptionCode
#define GetExceptionCode()      0
#endif
#ifndef EXCEPTION_EXECUTE_HANDLER
#define EXCEPTION_EXECUTE_HANDLER 1
#endif
#ifndef EXCEPTION_CONTINUE_SEARCH
#define EXCEPTION_CONTINUE_SEARCH 0
#endif
#endif /* __GNUC__ */

#include <ntifs.h>
#include <ntdddisk.h>

#include "hpfsdisk.h"
#include "hpfssup.h"

/* ------------------------------------------------------------------ */
/* NT 3.1 import profile                                               */
/* ------------------------------------------------------------------ */

/*
 * Windows NT 3.1 does not export the Lite executive-resource functions,
 * MmCanFileBeTruncated, or the FsRtlNotifyFull / NotifySync change-
 * notification family (all introduced in NT 3.5/4.0). Mapping the Lite
 * calls to their originals is exact; truncation always allowed is the safe
 * choice where the memory manager cannot answer; change notification is an
 * optional feature and is left out on NT 3.1. One binary then loads on NT
 * 3.1 through 4.0. (NT 3.51 and later export the Lite set, so a build
 * without HPFS_NT31 uses it directly.)
 */
#ifdef HPFS_NT31

/*
 * The mingw DDK aliases the legacy resource names to their Lite forms (the
 * only ones modern ntoskrnl exports). Undo that and import the executive-
 * resource functions that are common to EVERY NT the one binary must load
 * on - 3.1, 3.51 and 4.0 - resolving the Lite names to them.
 *
 * The catch is the shared acquire: NT 3.1 exports ExAcquireResourceShared
 * but not the Lite form, while NT 3.51+ export ExAcquireResourceSharedLite
 * but DROPPED ExAcquireResourceShared. There is no shared-acquire name
 * common to all three, and an import the running kernel cannot resolve makes
 * the OS loader reject the driver as "missing or corrupt" (this is exactly
 * why the single binary failed to load on NT 3.51). The exclusive acquire,
 * by contrast, is exported under the same name (ExAcquireResourceExclusive)
 * by every one of them. So the one binary acquires exclusively wherever the
 * code asks for shared: exclusive is always a safe substitute for shared
 * (it is strictly more restrictive, the lock hierarchy stays acyclic, so no
 * new deadlock is possible), at the only cost of serialising concurrent
 * readers - immaterial on the uniprocessor retro systems this targets.
 * (A Visual C++ build pinned to a single NT version can instead import that
 * version's real shared acquire and keep true reader concurrency.)
 */
#undef ExAcquireResourceExclusive
#undef ExAcquireResourceShared
#undef ExReleaseResourceForThread
#undef ExInitializeResource
#undef ExDeleteResource
#undef ExAcquireResourceExclusiveLite
#undef ExAcquireResourceSharedLite
#undef ExReleaseResourceForThreadLite
#undef ExInitializeResourceLite
#undef ExDeleteResourceLite

/*
 * Resolved at DriverEntry from the running ntoskrnl (see hpfsres.c): the Lite
 * form on NT 3.51+, the legacy form on NT 3.1. The code calls the Lite names;
 * they expand to calls through these pointers. (Init/Delete return NTSTATUS in
 * the Lite ABI and VOID in the legacy one; the pointer type is NTSTATUS and
 * the legacy return is simply ignored - harmless on stdcall x86.)
 */
typedef NTSTATUS (NTAPI *HPFS_PINITRES)(PERESOURCE);
typedef BOOLEAN  (NTAPI *HPFS_PACQRES)(PERESOURCE, BOOLEAN);
typedef VOID     (NTAPI *HPFS_PRELRES)(PERESOURCE, ERESOURCE_THREAD);
typedef NTSTATUS (NTAPI *HPFS_PDELRES)(PERESOURCE);

extern HPFS_PINITRES HpfspInitRes;
extern HPFS_PACQRES  HpfspAcqExcl;
extern HPFS_PACQRES  HpfspAcqShared;
extern HPFS_PDELRES  HpfspDelRes;
extern HPFS_PRELRES  HpfspRelForThread;

BOOLEAN HpfsResolveResourceApi(VOID);

#define ExInitializeResourceLite(r)          HpfspInitRes(r)
#define ExAcquireResourceExclusiveLite(r,w)  HpfspAcqExcl((r),(w))
#define ExAcquireResourceSharedLite(r,w)     HpfspAcqShared((r),(w))
#define ExDeleteResourceLite(r)              HpfspDelRes(r)
#define ExReleaseResourceForThreadLite(r,t)  HpfspRelForThread((r),(t))

/*
 * NT 4.0 introduced the fast-call Iof/Obf forms; NT 3.1 has only the
 * stdcall originals. The DDK maps the plain names to the fast-call ones,
 * so undo that and declare the stdcall exports NT 3.1 actually has.
 */
#undef IoCallDriver
#undef IoCompleteRequest
#undef ObDereferenceObject
__declspec(dllimport) NTSTATUS NTAPI IoCallDriver(PDEVICE_OBJECT, PIRP);
__declspec(dllimport) VOID     NTAPI IoCompleteRequest(PIRP, CCHAR);
__declspec(dllimport) VOID     NTAPI ObDereferenceObject(PVOID);

#define MmCanFileBeTruncated(S, N)      TRUE
#define FsRtlNotifyInitializeSync(S)    ((void)0)
#define FsRtlNotifyUninitializeSync(S)  ((void)0)
#define FsRtlNotifyCleanup(a, b, c)     ((void)0)
#define FsRtlNotifyFullChangeDirectory(a,b,c,d,e,f,g,h,i,j)  ((void)0)
#define FsRtlNotifyFullReportChange(a,b,c,d,e,f,g,h,i)       ((void)0)
#endif /* HPFS_NT31 */

#ifdef __GNUC__
/*
 * The mingw DDK IO_STACK_LOCATION already carries the file-system members,
 * so the HPFS_NT4 extended-stack cast is a plain identity there.
 */
typedef IO_STACK_LOCATION EXTENDED_IO_STACK_LOCATION, *PEXTENDED_IO_STACK_LOCATION;

/* The DDK headers redefine these SEH intrinsics; re-assert the shim so no
   _abnormal_termination / exception-record references are emitted. */
#undef AbnormalTermination
#define AbnormalTermination()       0
#undef GetExceptionCode
#define GetExceptionCode()          0
#undef GetExceptionInformation
#define GetExceptionInformation()   ((PEXCEPTION_POINTERS)0)
#endif

#ifdef HPFS_CROSS
/*
 * NT 3.1 does not export ExAllocatePoolWithTag, the Ke critical-region
 * calls, or the top-level-IRP / verify / requestor helpers (newer builds
 * access them inline through the thread). The universal binary drops the
 * pool tag, makes the critical region a no-op pair, and keeps the top-level
 * IRP in one process-wide slot. A Visual C++ NT 3.1 build would instead use
 * the real inline thread-field accessors.
 */
#define ExAllocatePoolWithTag(Type, Size, Tag)  ExAllocatePool((Type), (Size))
#define KeEnterCriticalRegion()                 ((void)0)
#define KeLeaveCriticalRegion()                 ((void)0)
extern PIRP HpfsXTopLevelIrp;
#define IoGetTopLevelIrp()          (HpfsXTopLevelIrp)
#define IoSetTopLevelIrp(I)         (HpfsXTopLevelIrp = (PIRP)(I))
#define IoGetRequestorProcess(Irp)  ((PEPROCESS)0)
#define IoGetDeviceToVerify(T)      ((PDEVICE_OBJECT)0)
#define IoSetDeviceToVerify(T, D)   ((void)0)
#endif

/* ------------------------------------------------------------------ */
/* Target configuration                                                */
/* ------------------------------------------------------------------ */

/*
 * The NT4 DDK IO_STACK_LOCATION lacks the file system members
 * (QueryDirectory, NotifyDirectory, FileSystemControl, LockControl);
 * the free ntifs.h provides them through EXTENDED_IO_STACK_LOCATION.
 */
#ifdef HPFS_NT4
#define HpfsXSp(IrpSp)           ((PEXTENDED_IO_STACK_LOCATION)(IrpSp))
#define HpfsMdlAddress(Mdl)      MmGetSystemAddressForMdl(Mdl)
#define HPFS_STATUS_DISMOUNTED   STATUS_FILE_INVALID
#else
#define HpfsXSp(IrpSp)           (IrpSp)
#define HpfsMdlAddress(Mdl)      MmGetSystemAddressForMdlSafe((Mdl), NormalPagePriority)
#define HPFS_STATUS_DISMOUNTED   STATUS_VOLUME_DISMOUNTED
#endif

/* Same as the W2K IoSkipCurrentIrpStackLocation, which NT4 lacks */
#define HpfsSkipStack(Irp) {                         \
    (Irp)->CurrentLocation++;                       \
    (Irp)->Tail.Overlay.CurrentStackLocation++;     \
}

#define HpfsCopyStackToNext(Irp) {                                           \
    PIO_STACK_LOCATION _Sp = IoGetCurrentIrpStackLocation(Irp);            \
    PIO_STACK_LOCATION _Next = IoGetNextIrpStackLocation(Irp);             \
    RtlCopyMemory(_Next, _Sp, FIELD_OFFSET(IO_STACK_LOCATION, CompletionRoutine)); \
    _Next->Control = 0;                                                     \
}

/* ExReleaseResourceLite is not exported by every NT4 build */
#define HpfsRelease(Resource) \
    ExReleaseResourceForThreadLite((Resource), ExGetCurrentResourceThread())

#ifndef VPB_LOCKED
#define VPB_LOCKED                  0x00000002
#endif

/* File system IRP details some DDK headers leave out */
#ifndef IRP_MN_QUERY_DIRECTORY
#define IRP_MN_QUERY_DIRECTORY          0x01
#endif
#ifndef IRP_MN_NOTIFY_CHANGE_DIRECTORY
#define IRP_MN_NOTIFY_CHANGE_DIRECTORY  0x02
#endif
#ifndef IRP_MN_USER_FS_REQUEST
#define IRP_MN_USER_FS_REQUEST          0x00
#endif
#ifndef IRP_MN_MOUNT_VOLUME
#define IRP_MN_MOUNT_VOLUME             0x01
#endif
#ifndef IRP_MN_VERIFY_VOLUME
#define IRP_MN_VERIFY_VOLUME            0x02
#endif
#ifndef SL_RESTART_SCAN
#define SL_RESTART_SCAN                 0x01
#endif
#ifndef SL_RETURN_SINGLE_ENTRY
#define SL_RETURN_SINGLE_ENTRY          0x02
#endif
#ifndef SL_INDEX_SPECIFIED
#define SL_INDEX_SPECIFIED              0x04
#endif
#ifndef SL_WATCH_TREE
#define SL_WATCH_TREE                   0x01
#endif
#ifndef SL_OPEN_PAGING_FILE
#define SL_OPEN_PAGING_FILE             0x02
#endif
#ifndef SL_OPEN_TARGET_DIRECTORY
#define SL_OPEN_TARGET_DIRECTORY        0x04
#endif
#ifndef SL_OVERRIDE_VERIFY_VOLUME
#define SL_OVERRIDE_VERIFY_VOLUME       0x02
#endif

#ifndef IO_REMOUNT
#define IO_REMOUNT                  0x00000001
#endif

#ifndef FILE_READ_ONLY_VOLUME
#define FILE_READ_ONLY_VOLUME       0x00080000
#endif

#ifndef FSCTL_IS_VOLUME_DIRTY
#define FSCTL_IS_VOLUME_DIRTY       CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 30, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#ifndef VOLUME_IS_DIRTY
#define VOLUME_IS_DIRTY             0x00000001
#endif

#ifndef FILE_WRITE_TO_END_OF_FILE
#define FILE_WRITE_TO_END_OF_FILE   0xffffffff
#endif

#ifndef IOCTL_DISK_IS_WRITABLE
#define IOCTL_DISK_IS_WRITABLE      CTL_CODE(IOCTL_DISK_BASE, 0x0009, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

/* Status codes newer than some DDK headers */
#ifndef STATUS_USER_MAPPED_FILE
#define STATUS_USER_MAPPED_FILE     ((NTSTATUS)0xC0000243L)
#endif
#ifndef STATUS_CANNOT_MAKE
#define STATUS_CANNOT_MAKE          ((NTSTATUS)0xC000016BL)
#endif

/* Information classes newer than NT4, answered on both targets */
#define HPFS_CLASS_ATTRIBUTE_TAG             35
#define HPFS_CLASS_FS_FULL_SIZE              7

typedef struct _HPFS_FILE_ATTRIBUTE_TAG_INFORMATION {
    ULONG       FileAttributes;
    ULONG       ReparseTag;
} HPFS_FILE_ATTRIBUTE_TAG_INFORMATION, *PHPFS_FILE_ATTRIBUTE_TAG_INFORMATION;

typedef struct _HPFS_FILE_FS_FULL_SIZE_INFORMATION {
    LARGE_INTEGER TotalAllocationUnits;
    LARGE_INTEGER CallerAvailableAllocationUnits;
    LARGE_INTEGER ActualAvailableAllocationUnits;
    ULONG       SectorsPerAllocationUnit;
    ULONG       BytesPerSector;
} HPFS_FILE_FS_FULL_SIZE_INFORMATION, *PHPFS_FILE_FS_FULL_SIZE_INFORMATION;

/* ------------------------------------------------------------------ */
/* Debug output                                                        */
/* ------------------------------------------------------------------ */

#define HPFS_PFX                 "[HPFSNT] "

#if DBG || defined(HPFS_DEBUG)
#define HPFS_DBG(args)           DbgPrint args
#else
#define HPFS_DBG(args)
#endif

/*
 * Debug trace to the QEMU "debugcon" port (0xE9): readable bytes with no
 * kernel-debugger protocol. Boot QEMU with -debugcon file:/path. Compile
 * with -DHPFS_DBGPORT to enable; a no-op otherwise.
 */
#ifdef HPFS_DBGPORT
static __inline void HpfsDbgCh(unsigned char c)
{ unsigned short port = 0x00E9;
  __asm__ __volatile__ ("outb %b0, %w1" : : "a"(c), "d"(port)); }
static __inline void HpfsDbgStr(const char *s)
{ while (*s) HpfsDbgCh((unsigned char)*s++); }
static __inline void HpfsDbgHex(ULONG_PTR v)
{ int i, n = (int)(sizeof(v) * 2); char h[17]; const char *d="0123456789abcdef";
  for (i=n-1;i>=0;i--){ h[i]=d[v&0xF]; v>>=4; } h[n]=0; HpfsDbgStr(h); }
static __inline void HpfsDbgUni(const void *us)
{ const unsigned short *p; unsigned short len, i;
  len = ((const UNICODE_STRING *)us)->Length;
  p   = (const unsigned short *)((const UNICODE_STRING *)us)->Buffer;
  if (!p) { HpfsDbgStr("(null)"); return; }
  for (i=0;i<len/2 && i<120;i++) HpfsDbgCh((unsigned char)(p[i]<128?p[i]:'?')); }
#define DBGP(s)     HpfsDbgStr(s)
#define DBGX(v)     HpfsDbgHex((ULONG_PTR)(v))
#define DBGU(u)     HpfsDbgUni(&(u))
#else
#define DBGP(s)     ((void)0)
#define DBGX(v)     ((void)0)
#define DBGU(u)     ((void)0)
#endif


/* ------------------------------------------------------------------ */
/* Structured exception handling                                       */
/* ------------------------------------------------------------------ */

/*
 * The Cache Manager, the Memory Manager and the probe routines report I/O
 * errors and bad user buffers by raising. Code that calls them runs that
 * part through HpfsTryCall (hpfsseh.c), which returns the exception code
 * (STATUS_SUCCESS when nothing was raised). With ExpectedOnly only the
 * codes FsRtlIsNtstatusExpected accepts (and in-page errors) are caught;
 * anything else is a bug and stops the system where it happened.
 */
typedef VOID (*HPFS_TRY_BODY)(PVOID Context);

NTSTATUS HpfsTryCall(HPFS_TRY_BODY Body, PVOID Context, BOOLEAN ExpectedOnly);

/* RtlCopyMemory to or from a caller's buffer, faults caught (mapped status) */
NTSTATUS HpfsSafeCopy(PVOID Destination, const VOID *Source, SIZE_T Length);

/* Maps an exception code to the status a request returns */
NTSTATUS HpfsExceptionStatus(NTSTATUS Code);

/* ------------------------------------------------------------------ */
/* Limits and tags                                                     */
/* ------------------------------------------------------------------ */

#define HPFS_FS_DEVICE_NAME     L"\\Pinball"
#define HPFS_FS_NAME            L"HPFS"

#define HPFS_TAG                'sfpH'
#define HPFS_TAG_FCB            'FfpH'
#define HPFS_TAG_CCB            'CfpH'
#define HPFS_TAG_RUNS           'RfpH'
#define HPFS_TAG_BUFFER         'BfpH'
#define HPFS_TAG_NAME           'NfpH'
#define HPFS_TAG_WORK           'WfpH'
#define HPFS_TAG_META           'MfpH'
#define HPFS_TAG_EA             'EfpH'
#define HPFS_TAG_ENGINE         'efpH'
#define HPFS_TAG_REGISTRY       'GfpH'

#define HPFS_PAGE_SIZE          0x1000
#define HPFS_ZERO_CHUNK         0x10000
#define HPFS_MAX_LABEL          11

/* ------------------------------------------------------------------ */
/* Node types                                                          */
/* ------------------------------------------------------------------ */

#define HPFS_NTC_VCB            ((CSHORT)0x0E01)
#define HPFS_NTC_FCB            ((CSHORT)0x0E02)    /* file */
#define HPFS_NTC_DCB            ((CSHORT)0x0E03)    /* directory */
#define HPFS_NTC_VFCB           ((CSHORT)0x0E04)    /* whole volume (DASD) */
#define HPFS_NTC_CCB            ((CSHORT)0x0E05)

#define HpfsNodeType(Ptr)       (*((CSHORT *)(Ptr)))

/* ------------------------------------------------------------------ */
/* Structures                                                          */
/* ------------------------------------------------------------------ */

typedef struct _HPFS_VCB HPFS_VCB, *PHPFS_VCB;

/*
 * NT 3.1's ERESOURCE is larger than later headers describe:
 * every embedded ERESOURCE is followed by padding.
 */
#define HPFS_ERESOURCE_PAD      0x40

typedef struct _HPFS_FCB {
    FSRTL_COMMON_FCB_HEADER Header;
    SECTION_OBJECT_POINTERS SectionObjectPointers;
    ERESOURCE   Resource;
    UCHAR       ResourcePad[HPFS_ERESOURCE_PAD];
    ERESOURCE   PagingIoResource;
    UCHAR       PagingIoResourcePad[HPFS_ERESOURCE_PAD];
    PHPFS_VCB   Vcb;
    struct _HPFS_FCB *ParentDcb;
    LIST_ENTRY  FcbLinks;
    ULONG       RefCount;           /* file objects + child FCBs */
    ULONG       UncleanCount;
    ULONG       NonCachedUncleanCount;
    ULONG       FcbState;
    SHARE_ACCESS ShareAccess;
    FILE_LOCK   FileLock;
    ULONG       FileLockSpare[16];  /* NT 3.1's FILE_LOCK is larger than the headers' */
    PKTHREAD    LazyWriteThread;

    ULONG       Fnode;              /* the file's fnode: its identity */
    ULONG       DirFnode;           /* fnode of the containing directory */
    UCHAR       Attributes;         /* HPFS (DOS) attribute byte */
    UCHAR       DirentFlags;        /* HPFS_DE_EA / HPFS_DE_NEEDEA / ACL bits */
    UCHAR       AclCount;
    UCHAR       CodePage;
    ULONG       EaSize;             /* bytes of EA list (dirent value) */
    ULONG       OemNameLength;
    UCHAR       OemName[HPFS_MAX_NAME + 1];     /* the name as stored */
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER LastAccessTime;
    UNICODE_STRING Name;            /* last component */
    UNICODE_STRING FullName;        /* built on demand */

    /* Allocation map, rebuilt from the fnode when it changes */
    PHPFS_RUN   Runs;
    ULONG       RunCount;
    ULONG       AllocSectors;
} HPFS_FCB, *PHPFS_FCB;

#define FCB_STATE_ROOT              0x0001
#define FCB_STATE_VISITED           0x0002
#define FCB_STATE_DELETE_PENDING    0x0004
#define FCB_STATE_DELETED           0x0008  /* off the disk, kept for Cc and Mm */
#define FCB_STATE_DIRENT_DIRTY      0x0010  /* the entry lags behind the FCB */
#define FCB_STATE_TRUNCATE_ON_CLOSE 0x0020
#define FCB_STATE_PAGING_FILE       0x0200

typedef struct _HPFS_CCB {
    CSHORT      NodeTypeCode;
    CSHORT      NodeByteSize;
    ULONG       Flags;
    ULONG       QueryState;         /* 0: ".", 1: "..", 2: entries */
    ULONG       LastNameLength;     /* entries: resume after this name */
    UCHAR       LastName[HPFS_MAX_NAME + 1];
    BOOLEAN     HaveLast;
    ULONG       EaIndex;            /* next EA for a query-EA continuation */
    UNICODE_STRING Pattern;         /* upcased */
} HPFS_CCB, *PHPFS_CCB;

#define CCB_FLAG_PATTERN_SET        0x0001
#define CCB_FLAG_WILDCARD           0x0002
#define CCB_FLAG_MATCH_ALL          0x0004
#define CCB_FLAG_VOLUME_OPEN        0x0008
#define CCB_FLAG_DISMOUNTED_VOLUME  0x0010
#define CCB_FLAG_DELETE_ON_CLOSE    0x0020
#define CCB_FLAG_USER_SET_WRITE     0x0040
#define CCB_FLAG_USER_SET_ACCESS    0x0080
#define CCB_FLAG_WRITE_HANDLE       0x0100

/* One cached metadata block (a run of sectors the engine maps) */
typedef struct _HPFS_META {
    LIST_ENTRY  Links;              /* LRU (most recent at the head) */
    struct _HPFS_META *HashNext;
    ULONG       Sector;
    ULONG       Count;
    ULONG       Refs;
    BOOLEAN     Dirty;
    BOOLEAN     Valid;
    PUCHAR      Data;
} HPFS_META, *PHPFS_META;

#define HPFS_META_HASH          256
#define HPFS_META_MAX_BYTES     (512 * 1024)    /* clean blocks kept */

struct _HPFS_VCB {
    CSHORT      NodeTypeCode;
    CSHORT      NodeByteSize;
    ERESOURCE   Resource;
    UCHAR       ResourcePad[HPFS_ERESOURCE_PAD];
    ERESOURCE   EngineResource;     /* one thread in the engine at a time */
    UCHAR       EngineResourcePad[HPFS_ERESOURCE_PAD];
    LIST_ENTRY  VcbLinks;
    PVPB        Vpb;
    PDEVICE_OBJECT TargetDeviceObject;
    PDEVICE_OBJECT VolumeDeviceObject;
    ULONG       VcbState;
    ULONG       OpenCount;
    ULONG       UncleanCount;
    ULONG       WriteCount;
    PFILE_OBJECT LockFileObject;
    PKTHREAD    VerifyThread;
    PHPFS_FCB   VolumeFcb;
    PHPFS_FCB   RootDcb;
    LIST_ENTRY  FcbList;

    HPFS_VOL    Vol;                /* engine state */
    LONGLONG    VolumeBytes;        /* file system size */
    LONGLONG    PartitionBytes;
    ULONG       SerialNumber;
    BOOLEAN     DirtyMarked;        /* we set the dirty bit on the disk */
    BOOLEAN     InMarkDirty;
    ULONG       LabelLength;        /* characters */
    WCHAR       Label[HPFS_MAX_LABEL];

    /* Metadata cache (under EngineResource) */
    LIST_ENTRY  MetaLru;
    PHPFS_META  MetaHash[HPFS_META_HASH];
    ULONG       MetaBytes;
    ULONG       MetaDirty;
    NTSTATUS    MetaWriteStatus;    /* first write error since the last flush */

    PNOTIFY_SYNC NotifySync;
    LIST_ENTRY  DirNotifyList;
};

#define VCB_STATE_MOUNTED           0x0001
#define VCB_STATE_LOCKED            0x0002
#define VCB_STATE_DISMOUNTED        0x0004
#define VCB_STATE_IN_DISMOUNT       0x0008
#define VCB_STATE_DELETE_PENDING    0x0010
#define VCB_STATE_DASD_WRITTEN      0x0020
#define VCB_STATE_REMOVABLE         0x0040
#define VCB_STATE_PNP_LOCKED        0x0080
#define VCB_STATE_FREE_VPB          0x0100
#define VCB_STATE_REMOVED           0x0200
#define VCB_STATE_READ_ONLY         0x0400
#define VCB_STATE_KEEP_DIRTY        0x0800
#define VCB_STATE_FLUSH_ON_CLOSE    0x1000
#define VCB_STATE_SHUTDOWN          0x2000

typedef struct _HPFS_DATA {
    PDRIVER_OBJECT  DriverObject;
    PDEVICE_OBJECT  FileSystemDeviceObject;
    ERESOURCE       Resource;
    UCHAR           ResourcePad[HPFS_ERESOURCE_PAD];
    LIST_ENTRY      VcbList;
    FAST_IO_DISPATCH FastIoDispatch;
    CACHE_MANAGER_CALLBACKS CacheManagerCallbacks;
    UNICODE_STRING  RegistryPath;
} HPFS_DATA, *PHPFS_DATA;

extern HPFS_DATA HpfsData;

typedef struct _HPFS_IRP_CONTEXT {
    PIRP            Irp;
    PIO_STACK_LOCATION IrpSp;
    PDEVICE_OBJECT  DeviceObject;
    PHPFS_VCB       Vcb;
    ULONG           Flags;
    NTSTATUS        ExceptionStatus;
    NTSTATUS        DispatchStatus;
} HPFS_IRP_CONTEXT, *PHPFS_IRP_CONTEXT;

#define HPFS_CTX_TOP_LEVEL          0x0001
#define HPFS_CTX_NO_COMPLETE        0x0002

typedef struct _HPFS_CLOSE_ITEM {
    WORK_QUEUE_ITEM Item;
    PHPFS_VCB   Vcb;
    PHPFS_FCB   Fcb;
    PHPFS_CCB   Ccb;
} HPFS_CLOSE_ITEM, *PHPFS_CLOSE_ITEM;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

#define HpfsIsFcb(Fcb)      ((Fcb)->Header.NodeTypeCode == HPFS_NTC_FCB)
#define HpfsIsDcb(Fcb)      ((Fcb)->Header.NodeTypeCode == HPFS_NTC_DCB)
#define HpfsIsVfcb(Fcb)     ((Fcb)->Header.NodeTypeCode == HPFS_NTC_VFCB)

#define HpfsRoundUp(Value, Size) \
    (((Value) + ((Size) - 1)) & ~((LONGLONG)(Size) - 1))

#define HPFS_WRITE_ACCESS   (FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | \
                             FILE_WRITE_ATTRIBUTES | DELETE | FILE_DELETE_CHILD)

#define HPFS_ATTR_SETTABLE  (HPFS_ATTR_READONLY | HPFS_ATTR_HIDDEN | HPFS_ATTR_SYSTEM | HPFS_ATTR_ARCHIVE)

/* ------------------------------------------------------------------ */
/* Prototypes                                                          */
/* ------------------------------------------------------------------ */

/* hpfsinit.c */
NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath);
BOOLEAN HpfsWriteSupportEnabled(VOID);

/* hpfsdisp.c */
NTSTATUS NTAPI HpfsFsdDispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS HpfsVerifyVcb(PHPFS_IRP_CONTEXT Ctx, PHPFS_VCB Vcb);
NTSTATUS HpfsVerifyWritable(PHPFS_IRP_CONTEXT Ctx, PHPFS_VCB Vcb);

/* hpfscrea.c */
NTSTATUS HpfsCommonCreate(PHPFS_IRP_CONTEXT Ctx);

/* hpfsclos.c */
NTSTATUS HpfsCommonCleanup(PHPFS_IRP_CONTEXT Ctx);
NTSTATUS HpfsCommonClose(PHPFS_IRP_CONTEXT Ctx);
BOOLEAN  HpfsTryTeardown(PHPFS_VCB Vcb, ULONG InFlightReferences);
VOID     HpfsDeleteVcb(PHPFS_VCB Vcb);
BOOLEAN  HpfsDismountVcb(PHPFS_VCB Vcb);
VOID     HpfsUnlockVcb(PHPFS_VCB Vcb);
VOID     HpfsPurgeCachedFiles(PHPFS_VCB Vcb);
NTSTATUS HpfsDeleteFromDisk(PHPFS_VCB Vcb, PHPFS_FCB Fcb);

/* hpfsread.c / hpfswrit.c */
NTSTATUS HpfsCommonRead(PHPFS_IRP_CONTEXT Ctx);
NTSTATUS HpfsCommonWrite(PHPFS_IRP_CONTEXT Ctx);
NTSTATUS HpfsZeroFileRange(PHPFS_FCB Fcb, PFILE_OBJECT FileObject, LONGLONG Start, LONGLONG End);

/* hpfsdir.c */
NTSTATUS HpfsCommonDirectoryControl(PHPFS_IRP_CONTEXT Ctx);
VOID     HpfsNotifyCleanupList(PHPFS_VCB Vcb, PHPFS_CCB Ccb);
VOID     HpfsNotifyChange(PHPFS_VCB Vcb, PHPFS_FCB Fcb, ULONG Filter, ULONG Action);

/* hpfsinfo.c */
NTSTATUS HpfsCommonQueryInformation(PHPFS_IRP_CONTEXT Ctx);
ULONG    HpfsNtAttributes(UCHAR Attributes);
VOID     HpfsFillBasicInfo(PHPFS_FCB Fcb, PFILE_BASIC_INFORMATION Info);
VOID     HpfsFillStandardInfo(PHPFS_FCB Fcb, PFILE_STANDARD_INFORMATION Info);
VOID     HpfsFillNetworkOpenInfo(PHPFS_FCB Fcb, PFILE_NETWORK_OPEN_INFORMATION Info);

/* hpfsseti.c */
NTSTATUS HpfsCommonSetInformation(PHPFS_IRP_CONTEXT Ctx);
NTSTATUS HpfsSetFileSize(PHPFS_IRP_CONTEXT Ctx, PHPFS_FCB Fcb, PFILE_OBJECT FileObject, LONGLONG NewSize);

/* hpfsea.c */
NTSTATUS HpfsCommonQueryEa(PHPFS_IRP_CONTEXT Ctx);
NTSTATUS HpfsCommonSetEa(PHPFS_IRP_CONTEXT Ctx);
NTSTATUS HpfsSetEaBuffer(PHPFS_VCB Vcb, PHPFS_FCB Fcb, PFILE_FULL_EA_INFORMATION Buffer, ULONG Length,
                         BOOLEAN Replace);
ULONG    HpfsNtEaSize(PHPFS_VCB Vcb, PHPFS_FCB Fcb);
ULONG    HpfsNtEaSizeOfFnode(PHPFS_VCB Vcb, ULONG Fnode);
NTSTATUS HpfsCheckEaBuffer(PFILE_FULL_EA_INFORMATION Buffer, ULONG Length, PULONG ErrorOffset);

/* hpfsvol.c */
NTSTATUS HpfsCommonQueryVolumeInformation(PHPFS_IRP_CONTEXT Ctx);
NTSTATUS HpfsCommonSetVolumeInformation(PHPFS_IRP_CONTEXT Ctx);

/* hpfsfsct.c */
NTSTATUS HpfsCommonFileSystemControl(PHPFS_IRP_CONTEXT Ctx);
NTSTATUS HpfsCommonPnp(PHPFS_IRP_CONTEXT Ctx);
NTSTATUS HpfsLoadVolume(PHPFS_VCB Vcb);
NTSTATUS HpfsUserFsRequest(PHPFS_IRP_CONTEXT Ctx);
VOID     HpfsLoadLabel(PHPFS_VCB Vcb);

/* hpfsmisc.c */
NTSTATUS HpfsCommonDeviceControl(PHPFS_IRP_CONTEXT Ctx);
NTSTATUS HpfsCommonLockControl(PHPFS_IRP_CONTEXT Ctx);

/* hpfsflsh.c */
NTSTATUS HpfsCommonFlushBuffers(PHPFS_IRP_CONTEXT Ctx);
NTSTATUS HpfsCommonShutdown(PHPFS_IRP_CONTEXT Ctx);
NTSTATUS HpfsFlushFile(PHPFS_VCB Vcb, PHPFS_FCB Fcb);
NTSTATUS HpfsFlushVolume(PHPFS_VCB Vcb, BOOLEAN MarkClean);
VOID     HpfsMarkVolumeDirty(PHPFS_VCB Vcb);
NTSTATUS HpfsMarkVolumeClean(PHPFS_VCB Vcb);

/* hpfsfast.c */
VOID     HpfsInitializeFastIo(PFAST_IO_DISPATCH FastIo);
VOID     HpfsInitializeCacheCallbacks(VOID);
UCHAR    HpfsIsFastIoPossible(PHPFS_FCB Fcb);

/* hpfsmeta.c: the engine's host and the metadata cache */
VOID     HpfsEngineEnter(PHPFS_VCB Vcb);
NTSTATUS HpfsEngineLeave(PHPFS_VCB Vcb, NTSTATUS Status);
NTSTATUS HpfsMetaFlush(PHPFS_VCB Vcb);
VOID     HpfsMetaPurge(PHPFS_VCB Vcb);
VOID     HpfsMetaInitialize(PHPFS_VCB Vcb);

/* hpfsio.c */
NTSTATUS HpfsSyncIo(PDEVICE_OBJECT Device, UCHAR MajorFunction, LONGLONG Offset,
                    ULONG Length, PVOID Buffer, BOOLEAN OverrideVerify);
NTSTATUS HpfsDeviceIoctl(PDEVICE_OBJECT Device, ULONG IoControlCode, PVOID InputBuffer,
                         ULONG InputLength, PVOID OutputBuffer, ULONG OutputLength,
                         BOOLEAN OverrideVerify, PULONG Information);
NTSTATUS HpfsFlushDevice(PHPFS_VCB Vcb);
NTSTATUS HpfsLockUserBuffer(PIRP Irp, LOCK_OPERATION Operation, ULONG Length);
PVOID    HpfsMapUserBuffer(PIRP Irp);
NTSTATUS HpfsNonCachedIo(PHPFS_IRP_CONTEXT Ctx, PHPFS_FCB Fcb, UCHAR MajorFunction,
                         LONGLONG StartingVbo, ULONG ByteCount, LONGLONG ValidData);
NTSTATUS HpfsZeroDisk(PHPFS_VCB Vcb, PHPFS_FCB Fcb, LONGLONG Start, LONGLONG End);
BOOLEAN  HpfsVboToLbo(PHPFS_VCB Vcb, PHPFS_FCB Fcb, LONGLONG Vbo, PLONGLONG Lbo, PULONG Contiguous);
NTSTATUS HpfsReadDisk(PHPFS_VCB Vcb, ULONG Sector, ULONG Count, PVOID Buffer);
NTSTATUS HpfsWriteDisk(PHPFS_VCB Vcb, ULONG Sector, ULONG Count, PVOID Buffer);

#define HpfsReadSectors(Device, Offset, Length, Buffer, Override) \
    HpfsSyncIo((Device), IRP_MJ_READ, (Offset), (Length), (Buffer), (Override))
#define HpfsWriteSectors(Device, Offset, Length, Buffer, Override) \
    HpfsSyncIo((Device), IRP_MJ_WRITE, (Offset), (Length), (Buffer), (Override))

/* hpfsstrc.c */
PHPFS_FCB HpfsCreateVolumeFcb(PHPFS_VCB Vcb);
PHPFS_FCB HpfsCreateRootDcb(PHPFS_VCB Vcb);
NTSTATUS HpfsCreateFcb(PHPFS_VCB Vcb, PHPFS_FCB ParentDcb, PHPFS_ENTRY Entry, PHPFS_FCB *Fcb);
VOID     HpfsDeleteFcb(PHPFS_FCB Fcb);
VOID     HpfsDereferenceFcb(PHPFS_VCB Vcb, PHPFS_FCB Fcb);
PHPFS_FCB HpfsFindFcbByFnode(PHPFS_VCB Vcb, ULONG Fnode);
PHPFS_CCB HpfsCreateCcb(VOID);
VOID     HpfsDeleteCcb(PHPFS_CCB Ccb);
NTSTATUS HpfsBuildFullName(PHPFS_FCB Fcb);
VOID     HpfsForgetNames(PHPFS_VCB Vcb, PHPFS_FCB Fcb);
BOOLEAN  HpfsIsAncestor(PHPFS_FCB Ancestor, PHPFS_FCB Fcb);
LARGE_INTEGER HpfsToNtTime(ULONG Seconds);
ULONG    HpfsFromNtTime(LARGE_INTEGER Time);
VOID     HpfsFcbToEntry(PHPFS_FCB Fcb, PHPFS_ENTRY Entry);
NTSTATUS HpfsUpdateDirent(PHPFS_VCB Vcb, PHPFS_FCB Fcb);
NTSTATUS HpfsLoadRuns(PHPFS_VCB Vcb, PHPFS_FCB Fcb);
NTSTATUS HpfsSetAllocation(PHPFS_VCB Vcb, PHPFS_FCB Fcb, ULONGLONG Bytes);
NTSTATUS HpfsUnicodeToOem(PCUNICODE_STRING Name, PUCHAR Oem, PULONG OemLength);
NTSTATUS HpfsOemToUnicode(const UCHAR *Oem, ULONG OemLength, PUNICODE_STRING Name);
BOOLEAN  HpfsIsValidUnicodeName(PUNICODE_STRING Name);

#endif /* _HPFS_H_ */
