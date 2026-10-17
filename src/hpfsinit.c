/*
 * HPFSNT - driver entry and settings
 *
 * Settings live in the service key and are read at every mount, so a
 * change applies to volumes mounted afterwards:
 *
 *   EnableWriteSupport (REG_DWORD)  1 or missing: read/write, 0: read-only
 */

#include "hpfs.h"

HPFS_DATA HpfsData;

/* hpfsinst tells this driver from the system's pinball.sys by this string */
#ifdef __GNUC__
__attribute__((used))
#endif
const char HpfsSignature[] = "HPFSNT file system driver";

#ifdef HPFS_CROSS
PIRP HpfsXTopLevelIrp;
#endif

/* Keeps a NUL-terminated copy of the service key path */
static VOID
HpfsSaveRegistryPath (
    PUNICODE_STRING RegistryPath
    )
{
    USHORT Size;

    if (RegistryPath == NULL || RegistryPath->Length == 0) {
        return;
    }

    Size = (USHORT)(RegistryPath->Length + sizeof(WCHAR));
    HpfsData.RegistryPath.Buffer = (PWCHAR)ExAllocatePoolWithTag(PagedPool, Size, HPFS_TAG_REGISTRY);

    if (HpfsData.RegistryPath.Buffer != NULL) {
        RtlCopyMemory(HpfsData.RegistryPath.Buffer, RegistryPath->Buffer, RegistryPath->Length);
        HpfsData.RegistryPath.Buffer[RegistryPath->Length / sizeof(WCHAR)] = 0;
        HpfsData.RegistryPath.Length = RegistryPath->Length;
        HpfsData.RegistryPath.MaximumLength = Size;
    }
}

/* EnableWriteSupport: nonzero (the default) lets volumes be written */
BOOLEAN
HpfsWriteSupportEnabled (
    VOID
    )
{
    RTL_QUERY_REGISTRY_TABLE Table[2];
    ULONG Value = 1;
    ULONG Default = 1;

    if (HpfsData.RegistryPath.Buffer == NULL) {
        return TRUE;
    }

    RtlZeroMemory(Table, sizeof(Table));
    Table[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
    Table[0].Name = L"EnableWriteSupport";
    Table[0].EntryContext = &Value;
    Table[0].DefaultType = REG_DWORD;
    Table[0].DefaultData = &Default;
    Table[0].DefaultLength = sizeof(ULONG);

    if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, HpfsData.RegistryPath.Buffer,
                                           Table, NULL, NULL))) {
        return TRUE;
    }

    return (BOOLEAN)(Value != 0);
}

#ifdef HPFS_SEHTEST
/* Self-test of HpfsTryCall (debug builds): two raises, both must be caught */
static VOID HpfsRaiseBody(PVOID Context) { ExRaiseStatus(*(PNTSTATUS)Context); }
static VOID HpfsNestedBody(PVOID Context) { (VOID)HpfsTryCall(HpfsRaiseBody, Context, TRUE); ExRaiseStatus(STATUS_DISK_FULL); }
static VOID
HpfsSehSelfTest (
    VOID
    )
{
    NTSTATUS Code = STATUS_INVALID_USER_BUFFER, R;
    R = HpfsTryCall(HpfsRaiseBody, &Code, FALSE);
    DBGP("[HPFS sehtest 1 "); DBGX(R); DBGP("]\n");
    Code = STATUS_IN_PAGE_ERROR;
    R = HpfsTryCall(HpfsNestedBody, &Code, TRUE);
    DBGP("[HPFS sehtest 2 "); DBGX(R); DBGP("]\n");
    R = HpfsSafeCopy(&Code, (PVOID)(ULONG_PTR)16, sizeof(Code));
    DBGP("[HPFS sehtest 3 "); DBGX(R); DBGP("]\n");
}
#endif

NTSTATUS
NTAPI
DriverEntry (
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath
    )
{
    UNICODE_STRING Name;
    PDEVICE_OBJECT DeviceObject;
    NTSTATUS Status;
    ULONG i;

#ifdef HPFS_NT31
    /* Bind the executive-resource API to the running kernel (Lite on NT 3.51+,
     * legacy on NT 3.1) before any resource is initialised. */
    if (!HpfsResolveResourceApi()) {
        DBGP("[HPFSNT:resource API not found]\n");
        return STATUS_PROCEDURE_NOT_FOUND;
    }
#endif

    RtlZeroMemory(&HpfsData, sizeof(HpfsData));

#ifdef HPFS_SEHTEST
    HpfsSehSelfTest();
#endif

    HpfsSaveRegistryPath(RegistryPath);

    RtlInitUnicodeString(&Name, HPFS_FS_DEVICE_NAME);

    Status = IoCreateDevice(DriverObject,
                            0,
                            &Name,
                            FILE_DEVICE_DISK_FILE_SYSTEM,
                            0,
                            FALSE,
                            &DeviceObject);

    if (!NT_SUCCESS(Status)) {
        HPFS_DBG((HPFS_PFX "IoCreateDevice failed %lX\n", Status));
        return Status;
    }

    HpfsData.DriverObject = DriverObject;
    HpfsData.FileSystemDeviceObject = DeviceObject;

    ExInitializeResourceLite(&HpfsData.Resource);
    InitializeListHead(&HpfsData.VcbList);

    /*
     * Wire the dispatch table. The DRIVER_OBJECT.MajorFunction[] array is
     * sized IRP_MJ_MAXIMUM_FUNCTION+1 by the running kernel, and that count
     * grew over time: NT 3.1 - 3.51 stop well below the value the modern
     * (WDK / mingw) headers define for IRP_MJ_MAXIMUM_FUNCTION (0x1b), so a
     * loop to the header's maximum writes past the end of the kernel's array
     * and corrupts the adjacent pool block. Only the low majors exist on NT
     * 3.1 anyway (no Power/WMI/PnP), so cap at the highest one HPFSNT actually
     * serves - IRP_MJ_CLEANUP (0x12), which every NT since 3.1 provides.
     */
#ifdef HPFS_NT31
    for (i = 0; i <= IRP_MJ_CLEANUP; i++) {
        DriverObject->MajorFunction[i] = HpfsFsdDispatch;
    }
#else
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        DriverObject->MajorFunction[i] = HpfsFsdDispatch;
    }
#endif

    HpfsInitializeFastIo(&HpfsData.FastIoDispatch);
    DriverObject->FastIoDispatch = &HpfsData.FastIoDispatch;

    HpfsInitializeCacheCallbacks();

#ifdef DO_DEVICE_INITIALIZING
    DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
#endif

    /* Volumes are left clean at shutdown */
    (VOID)IoRegisterShutdownNotification(DeviceObject);

    IoRegisterFileSystem(DeviceObject);

    HPFS_DBG((HPFS_PFX "Loaded\n"));

    return STATUS_SUCCESS;
}
