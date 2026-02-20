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
#include <shellapi.h>
#include <stdio.h>

#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "shell32.lib")

#define CHECK_INTERVAL 500
#define BOOT_DELAY     15000
#define MUTEX_NAME     "Global\\NVBypassMutex"
#define REG_SUBKEY     "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run"
#define REG_VALUE      "NVBypass"

#define WM_TRAYICON    (WM_USER + 1)
#define ID_TRAY_SHOW   1001
#define ID_TRAY_AUTO   1002
#define ID_TRAY_QUIT   1003

typedef NTSTATUS(WINAPI* pfnNtQIP)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
static pfnNtQIP pNtQIP = nullptr;

static NOTIFYICONDATAA nid = {};
static HWND g_hwnd = nullptr;
static HWND g_console = nullptr;
static bool g_consoleVisible = true;
static bool g_running = true;

// ---- Browsers ----

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

// ---- Command line reader ----

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

// ---- CDM killer ----

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

// ---- Auto-start ----

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

// ---- Tray icon ----

static void AddTrayIcon(HWND hwnd)
{
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    strcpy_s(nid.szTip, "NVIDIA ShadowPlay DRM Bypass");

    Shell_NotifyIconA(NIM_ADD, &nid);
}

static void RemoveTrayIcon()
{
    Shell_NotifyIconA(NIM_DELETE, &nid);
}

static void ShowTrayMenu(HWND hwnd)
{
    POINT pt;
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();

    if (g_consoleVisible)
        AppendMenuA(menu, MF_STRING, ID_TRAY_SHOW, "Hide");
    else
        AppendMenuA(menu, MF_STRING, ID_TRAY_SHOW, "Show");

    AppendMenuA(menu, MF_STRING | (HasAutoStart() ? MF_CHECKED : 0), ID_TRAY_AUTO, "Auto-start");
    AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(menu, MF_STRING, ID_TRAY_QUIT, "Quit");

    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_TRAYICON:
        if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP)
            ShowTrayMenu(hwnd);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case ID_TRAY_SHOW:
            g_consoleVisible = !g_consoleVisible;
            ShowWindow(g_console, g_consoleVisible ? SW_SHOW : SW_HIDE);
            break;

        case ID_TRAY_AUTO:
            ToggleAutoStart();
            break;

        case ID_TRAY_QUIT:
            g_running = false;
            RemoveTrayIcon();
            PostQuitMessage(0);
            break;
        }
        return 0;

    case WM_DESTROY:
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wp, lp);
}

static HWND CreateMessageWindow()
{
    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "NVBypassTray";

    RegisterClassA(&wc);

    return CreateWindowA(wc.lpszClassName, "", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
}

// ---- Worker thread ----

static DWORD WINAPI WorkerThread(LPVOID param)
{
    bool silent = *(bool*)param;
    int total = 0;

    while (g_running)
    {
        int k = KillCDM(!silent);

        if (k > 0)
        {
            total += k;

            if (!silent)
                printf("Total: %d\n", total);
        }

        Sleep(CHECK_INTERVAL);
    }

    return 0;
}

// ---- Entry ----

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

    g_console = GetConsoleWindow();

    if (silent)
    {
        ShowWindow(g_console, SW_HIDE);
        g_consoleVisible = false;
        Sleep(BOOT_DELAY);
    }

    if (!silent)
    {
        printf("NVIDIA ShadowPlay DRM Bypass\n");
        printf("Auto-start: %s (toggle with --autostart)\n", HasAutoStart() ? "ON" : "OFF");
        printf("Monitoring CDM processes. Ctrl+C to quit.\n\n");
    }

    // Create tray icon
    g_hwnd = CreateMessageWindow();
    AddTrayIcon(g_hwnd);

    // Start worker thread
    HANDLE hThread = CreateThread(nullptr, 0, WorkerThread, &silent, 0, nullptr);

    // Message loop
    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    g_running = false;
    WaitForSingleObject(hThread, 2000);
    CloseHandle(hThread);

    ReleaseMutex(mx);
    CloseHandle(mx);
    return 0;
}
