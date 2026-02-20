/*
 * NVIDIA ShadowPlay DRM Bypass
 * github.com/lithellx
 *
 * Fixes ShadowPlay/Instant Replay being disabled when DRM sites
 * (Spotify Web, Netflix, Kick, etc.) are open in a browser.
 *
 * Kills browser Widevine CDM processes that trigger NVIDIA's
 * output protection. Playback is unaffected.
 *
 * Run as Administrator.
 */

#include <windows.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <stdio.h>

#pragma comment(lib, "ntdll.lib")

#define CHECK_INTERVAL 500
#define BOOT_DELAY     15000
#define MUTEX_NAME     "Global\\NVBypassMutex"
#define REG_SUBKEY     "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run"
#define REG_VALUE      "NVBypass"

typedef NTSTATUS(WINAPI* pfnNtQIP)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
static pfnNtQIP pNtQIP = nullptr;

static const wchar_t* BROWSERS[] =
{
    L"chrome.exe",
    L"msedge.exe",
    L"msedgewebview2.exe",
    L"firefox.exe",
    L"opera.exe",
    L"brave.exe",
    L"vivaldi.exe",
    nullptr
};

static bool IsBrowser(const wchar_t* name)
{
    for (int i = 0; BROWSERS[i]; i++)
    {
        if (_wcsicmp(name, BROWSERS[i]) == 0)
            return true;
    }
    return false;
}

static bool ReadCmdLine(DWORD pid, wchar_t* out, size_t max)
{
    out[0] = 0;

    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h)
        return false;

    PROCESS_BASIC_INFORMATION pbi = {};
    ULONG len = 0;

    if (pNtQIP(h, ProcessBasicInformation, &pbi, sizeof(pbi), &len) != 0 || !pbi.PebBaseAddress)
    {
        CloseHandle(h);
        return false;
    }

    PEB peb = {};
    SIZE_T rd = 0;

    if (!ReadProcessMemory(h, pbi.PebBaseAddress, &peb, sizeof(peb), &rd))
    {
        CloseHandle(h);
        return false;
    }

    RTL_USER_PROCESS_PARAMETERS params = {};

    if (!ReadProcessMemory(h, peb.ProcessParameters, &params, sizeof(params), &rd))
    {
        CloseHandle(h);
        return false;
    }

    USHORT chars = params.CommandLine.Length / sizeof(wchar_t);

    if (chars == 0 || chars >= max)
    {
        CloseHandle(h);
        return false;
    }

    if (!ReadProcessMemory(h, params.CommandLine.Buffer, out, chars * sizeof(wchar_t), &rd))
    {
        CloseHandle(h);
        return false;
    }

    out[chars] = 0;
    CloseHandle(h);
    return true;
}

static int KillCDM(bool log)
{
    int killed = 0;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    wchar_t cmd[4096];

    if (Process32FirstW(snap, &pe))
    {
        do
        {
            if (!IsBrowser(pe.szExeFile))
                continue;

            if (!ReadCmdLine(pe.th32ProcessID, cmd, 4096))
                continue;

            if (!wcsstr(cmd, L"cdm") && !wcsstr(cmd, L"CDM") && !wcsstr(cmd, L"CdmService"))
                continue;

            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
            if (h)
            {
                if (TerminateProcess(h, 0))
                {
                    killed++;
                    if (log)
                        printf("[kill] %ls (PID %lu)\n", pe.szExeFile, pe.th32ProcessID);
                }
                CloseHandle(h);
            }
        } while (Process32NextW(snap, &pe));
    }

    CloseHandle(snap);
    return killed;
}

static bool HasAutoStart()
{
    HKEY k;
    bool ok = false;

    if (RegOpenKeyExA(HKEY_CURRENT_USER, REG_SUBKEY, 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS)
    {
        ok = RegQueryValueExA(k, REG_VALUE, 0, 0, 0, 0) == ERROR_SUCCESS;
        RegCloseKey(k);
    }

    return ok;
}

static void ToggleAutoStart()
{
    HKEY k;

    if (HasAutoStart())
    {
        if (RegOpenKeyExA(HKEY_CURRENT_USER, REG_SUBKEY, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS)
        {
            RegDeleteValueA(k, REG_VALUE);
            RegCloseKey(k);
            printf("Auto-start disabled.\n");
        }
    }
    else
    {
        char path[MAX_PATH], cmd[MAX_PATH + 16];
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        snprintf(cmd, sizeof(cmd), "\"%s\" --silent", path);

        if (RegOpenKeyExA(HKEY_CURRENT_USER, REG_SUBKEY, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS)
        {
            RegSetValueExA(k, REG_VALUE, 0, REG_SZ, (BYTE*)cmd, (DWORD)strlen(cmd) + 1);
            RegCloseKey(k);
            printf("Auto-start enabled.\n");
        }
    }
}

int main(int argc, char** argv)
{
    bool silent = false;

    SetConsoleTitleA("NVIDIA ShadowPlay DRM Bypass");

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--silent") == 0)
            silent = true;

        if (strcmp(argv[i], "--autostart") == 0)
        {
            ToggleAutoStart();
            return 0;
        }
    }

    pNtQIP = (pfnNtQIP)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");
    if (!pNtQIP)
        return 1;

    HANDLE mx = CreateMutexA(nullptr, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;

    if (silent)
    {
        FreeConsole();
        Sleep(BOOT_DELAY);
    }

    if (!silent)
    {
        printf("NVIDIA ShadowPlay DRM Bypass\n");
        printf("Auto-start: %s (toggle with --autostart)\n", HasAutoStart() ? "ON" : "OFF");
        printf("Monitoring CDM processes. Ctrl+C to quit.\n");
    }

    int total = 0;
    while (true)
    {
        int k = KillCDM(!silent);
        if (k > 0)
        {
            total += k;
            if (!silent)
                printf("Total: %d\n\n", total);
        }

        Sleep(CHECK_INTERVAL);
    }

    ReleaseMutex(mx);
    CloseHandle(mx);
    return 0;
}