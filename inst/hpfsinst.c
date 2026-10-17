/*
 * HPFSNT - hpfsinst: installs or removes the HPFSNT file system driver.
 *
 *   hpfsinst           Install: back up the system's pinball.sys (if any) to
 *                      pinball.sav and put HPFSNT's pinball.sys in its place,
 *                      and make sure the Pinball service exists and starts.
 *                      On NT 3.1 - 3.51 HPFSNT then serves every HPFS volume,
 *                      the boot volume included (the OS loader loads the boot
 *                      file system as pinball.sys). NT 4.0, 2000 and XP have
 *                      no HPFS driver: there the service is created.
 *   hpfsinst /BOOT     Also make the driver boot-start (for booting Windows
 *                      NT 4.0 or later from an HPFS volume, with an NTLDR
 *                      that can read HPFS).
 *   hpfsinst /READONLY Mount HPFS volumes read-only.
 *   hpfsinst /U        Uninstall: restore pinball.sav (or disable the
 *                      service this installer created).
 *
 * Put pinball.sys next to hpfsinst.exe. A restart applies the change.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#define SVC_PINBALL         "SYSTEM\\CurrentControlSet\\Services\\Pinball"

#define SERVICE_BOOT_START      0
#define SERVICE_SYSTEM_START    1
#define SERVICE_DEMAND_START    3
#define SERVICE_DISABLED        4
#define SERVICE_FILE_SYSTEM     2

static LONG SetDword(HKEY Key, const char *Name, DWORD Value)
{
    return RegSetValueExA(Key, Name, 0, REG_DWORD, (const BYTE *)&Value, sizeof(Value));
}

static LONG SetSz(HKEY Key, const char *Name, const char *Value)
{
    return RegSetValueExA(Key, Name, 0, REG_SZ, (const BYTE *)Value, (DWORD)(strlen(Value) + 1));
}

static DWORD GetDword(HKEY Key, const char *Name, DWORD Default)
{
    DWORD Type, Value, Size = sizeof(Value);
    if (RegQueryValueExA(Key, Name, NULL, &Type, (BYTE *)&Value, &Size) == ERROR_SUCCESS &&
        Type == REG_DWORD) {
        return Value;
    }
    return Default;
}

/* TRUE if the file is HPFSNT's driver (it carries this string) */
static int IsHpfsnt(const char *Path)
{
    static const char Mark[] = "HPFSNT file system driver";
    HANDLE H;
    DWORD Size, Got, I, N = sizeof(Mark) - 1;
    char *Buf;
    int Found = 0;

    H = CreateFileA(Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (H == INVALID_HANDLE_VALUE) return 0;
    Size = GetFileSize(H, NULL);
    if (Size != 0xFFFFFFFF && Size < 4 * 1024 * 1024) {
        Buf = (char *)HeapAlloc(GetProcessHeap(), 0, Size + 1);
        if (Buf != NULL) {
            if (ReadFile(H, Buf, Size, &Got, NULL) && Got == Size) {
                for (I = 0; I + N <= Size && !Found; I++) {
                    if (Buf[I] == Mark[0] && memcmp(Buf + I, Mark, N) == 0) Found = 1;
                }
            }
            HeapFree(GetProcessHeap(), 0, Buf);
        }
    }
    CloseHandle(H);
    return Found;
}

static void DriversPath(const char *Leaf, char *Out)
{
    char Dir[MAX_PATH];
    UINT N = GetWindowsDirectoryA(Dir, sizeof(Dir));
    if (N == 0 || N >= sizeof(Dir)) {
        Out[0] = 0;
        return;
    }
    sprintf(Out, "%s\\System32\\drivers\\%s", Dir, Leaf);
}

static int Install(int Boot, int ReadOnly)
{
    char Sys[MAX_PATH], Sav[MAX_PATH];
    HKEY Key;
    DWORD Disp, Start;
    LONG R;

    DriversPath("pinball.sys", Sys);
    DriversPath("pinball.sav", Sav);
    if (Sys[0] == 0) {
        printf("  cannot find the system directory.\n");
        return 1;
    }

    /* Keep the system's own driver once (never a copy of HPFSNT) */
    if (GetFileAttributesA(Sav) == 0xFFFFFFFF && GetFileAttributesA(Sys) != 0xFFFFFFFF && !IsHpfsnt(Sys)) {
        if (CopyFileA(Sys, Sav, TRUE)) {
            printf("  backed up the system's pinball.sys to pinball.sav\n");
        } else {
            printf("  warning: could not back up pinball.sys (error %lu).\n", GetLastError());
        }
    }

    if (!CopyFileA("pinball.sys", Sys, FALSE)) {
        printf("  error: could not copy pinball.sys to %s (error %lu).\n", Sys, GetLastError());
        printf("  put HPFSNT's pinball.sys next to hpfsinst.exe and run it again.\n");
        return 1;
    }
    printf("  installed HPFSNT as %s\n", Sys);

    R = RegCreateKeyExA(HKEY_LOCAL_MACHINE, SVC_PINBALL, 0, NULL, REG_OPTION_NON_VOLATILE,
                        KEY_ALL_ACCESS, NULL, &Key, &Disp);
    if (R != ERROR_SUCCESS) {
        printf("  error: cannot open the Pinball service key (error %ld).\n", R);
        return 1;
    }

    if (Disp == REG_CREATED_NEW_KEY) {
        /* NT 4.0 and later: no HPFS driver and no HPFS recognizer, so the
           driver starts with the system and mounts HPFS volumes itself */
        SetDword(Key, "Type", SERVICE_FILE_SYSTEM);
        SetDword(Key, "ErrorControl", 1);
        SetSz(Key, "ImagePath", "System32\\DRIVERS\\pinball.sys");
        SetDword(Key, "HpfsntCreated", 1);
        Start = SERVICE_SYSTEM_START;
        printf("  created the Pinball service\n");
    } else {
        Start = GetDword(Key, "Start", SERVICE_SYSTEM_START);
        if (Start == SERVICE_DISABLED) {
            Start = SERVICE_SYSTEM_START;
        }
    }
    if (Boot) {
        Start = SERVICE_BOOT_START;
    }
    SetDword(Key, "Start", Start);
    SetSz(Key, "Group", Start == SERVICE_BOOT_START ? "Boot file system" : "File system");
    SetDword(Key, "EnableWriteSupport", ReadOnly ? 0 : 1);
    RegFlushKey(Key);
    RegCloseKey(Key);

    printf("  Pinball service: %s start, %s\n",
           Start == SERVICE_BOOT_START ? "boot" : Start == SERVICE_SYSTEM_START ? "system" : "on demand",
           ReadOnly ? "read-only" : "read/write");
    printf("Done. Restart to load HPFSNT.\n");
    return 0;
}

static int Uninstall(void)
{
    char Sys[MAX_PATH], Sav[MAX_PATH];
    HKEY Key;

    DriversPath("pinball.sys", Sys);
    DriversPath("pinball.sav", Sav);

    if (Sys[0] && GetFileAttributesA(Sav) != 0xFFFFFFFF && !IsHpfsnt(Sav)) {
        if (CopyFileA(Sav, Sys, FALSE)) {
            printf("  restored the system's pinball.sys from pinball.sav\n");
        } else {
            printf("  warning: could not restore pinball.sys (error %lu).\n", GetLastError());
        }
    }

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, SVC_PINBALL, 0, KEY_ALL_ACCESS, &Key) == ERROR_SUCCESS) {
        if (GetDword(Key, "HpfsntCreated", 0)) {
            SetDword(Key, "Start", SERVICE_DISABLED);
            printf("  disabled the Pinball service HPFSNT created\n");
            if (Sys[0] && IsHpfsnt(Sys)) {
                DeleteFileA(Sys);
            }
        } else if (GetDword(Key, "Start", SERVICE_SYSTEM_START) == SERVICE_BOOT_START) {
            SetDword(Key, "Start", SERVICE_SYSTEM_START);
            SetSz(Key, "Group", "File system");
        }
        RegDeleteValueA(Key, "EnableWriteSupport");
        RegFlushKey(Key);
        RegCloseKey(Key);
    }

    printf("Done. Restart to apply.\n");
    return 0;
}

/* A 32-bit installer on 64-bit Windows would write into SysWOW64 */
static int UnderWow64(void)
{
    typedef BOOL (WINAPI *ISWOW64)(HANDLE, PBOOL);
    HMODULE K = GetModuleHandleA("kernel32.dll");
    ISWOW64 F = K ? (ISWOW64)GetProcAddress(K, "IsWow64Process") : NULL;
    BOOL W = FALSE;
    return F != NULL && F(GetCurrentProcess(), &W) && W;
}

int main(int argc, char **argv)
{
    int Boot = 0, ReadOnly = 0, I;

    for (I = 1; I < argc; I++) {
        if (!_stricmp(argv[I], "/U") || !_stricmp(argv[I], "-U")) {
            printf("HPFSNT installer\n");
            if (UnderWow64()) {
                printf("  this is the 32-bit installer: on 64-bit Windows run amd64\\hpfsinst.exe\n");
                return 1;
            }
            return Uninstall();
        } else if (!_stricmp(argv[I], "/BOOT")) {
            Boot = 1;
        } else if (!_stricmp(argv[I], "/READONLY")) {
            ReadOnly = 1;
        } else {
            printf("usage: hpfsinst [/BOOT] [/READONLY] | /U\n");
            return 2;
        }
    }

    printf("HPFSNT installer\n");
    if (UnderWow64()) {
        printf("  this is the 32-bit installer: on 64-bit Windows run amd64\\hpfsinst.exe\n");
        return 1;
    }
    return Install(Boot, ReadOnly);
}
