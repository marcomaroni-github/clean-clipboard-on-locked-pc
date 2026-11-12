/*
 * CleanClipboardOnLock - Ultra-lightweight Windows Clipboard Cleaner on Session Lock
 *
 * Pure Win32 C (No C Runtime / CRT).
 * - ~0% CPU (kernel event-driven via WTSRegisterSessionNotification + GetMessageW)
 * - ~0.3 - 0.5 MB Private Working Set RAM
 * - Clears both standard Win32 Clipboard (Ctrl+V) AND Windows 10/11 Clipboard History (Win+V)
 * - Completely invisible in normal operation
 *
 * Command-line switches:
 *   (none)        Run invisible background listener (single instance)
 *   --once        Clear clipboard + history immediately and exit (for Task Scheduler)
 *   --install     Register in HKCU\...\Run for auto-start at login and launch background process
 *   --uninstall   Stop running instance and remove auto-start registry entry
 *   --stop        Silently stop any running background instance
 *   --help / /?   Show usage message box
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <wtsapi32.h>

#define APP_NAME          L"CleanClipboardOnLock"
#define WINDOW_CLASS_NAME L"CleanClipboardOnLock_MsgClass"
#define WINDOW_TITLE      L"CleanClipboardOnLock_MsgWnd"
#define MUTEX_NAME        L"Local\\CleanClipboardOnLock_SingleInstance"
#define RUN_REG_KEY       L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

/* Compiler intrinsic fallbacks when building with /NODEFAULTLIB */
#pragma function(memset)
void* __cdecl memset(void* dest, int c, size_t count)
{
    volatile unsigned char* p = (volatile unsigned char*)dest;
    while (count--) {
        *p++ = (unsigned char)c;
    }
    return dest;
}

#pragma function(memcpy)
void* __cdecl memcpy(void* dest, const void* src, size_t count)
{
    volatile unsigned char* d = (volatile unsigned char*)dest;
    const volatile unsigned char* s = (const volatile unsigned char*)src;
    while (count--) {
        *d++ = *s++;
    }
    return dest;
}

static void ZeroMem(void* ptr, SIZE_T len)
{
    volatile unsigned char* p = (volatile unsigned char*)ptr;
    while (len--) {
        *p++ = 0;
    }
}

static UINT32 StrLenW(LPCWSTR s)
{
    UINT32 n = 0;
    if (!s) return 0;
    while (s[n] != L'\0') {
        ++n;
    }
    return n;
}

static BOOL IsSpaceW(WCHAR c)
{
    return (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n');
}

static WCHAR ToLowerAsciiW(WCHAR c)
{
    if (c >= L'A' && c <= L'Z') {
        return (WCHAR)(c + (L'a' - L'A'));
    }
    return c;
}

/* Matches exact CLI token (case-insensitive) after the executable name */
static BOOL HasCliFlag(LPCWSTR cmdLine, LPCWSTR flag)
{
    if (!cmdLine || !flag) return FALSE;

    LPCWSTR p = cmdLine;
    while (IsSpaceW(*p)) ++p;

    /* Skip argv[0] (quoted or unquoted executable path) */
    if (*p == L'"') {
        ++p;
        while (*p && *p != L'"') ++p;
        if (*p == L'"') ++p;
    } else {
        while (*p && !IsSpaceW(*p)) ++p;
    }

    UINT32 flagLen = StrLenW(flag);
    while (*p) {
        while (IsSpaceW(*p)) ++p;
        if (!*p) break;

        LPCWSTR tokenStart = p;
        while (*p && !IsSpaceW(*p)) ++p;
        UINT32 tokenLen = (UINT32)(p - tokenStart);

        if (tokenLen == flagLen) {
            BOOL match = TRUE;
            for (UINT32 i = 0; i < flagLen; ++i) {
                if (ToLowerAsciiW(tokenStart[i]) != ToLowerAsciiW(flag[i])) {
                    match = FALSE;
                    break;
                }
            }
            if (match) return TRUE;
        }
    }
    return FALSE;
}

/* -------------------------------------------------------------------------
 * WinRT Minimal ABI Definitions for Windows.ApplicationModel.DataTransfer.Clipboard
 * Interface: IClipboardStatics2 {d2ac1b6a-d29f-554b-b303-f0452345fe02}
 * Loaded dynamically from combase.dll ONLY during lock cleanup so zero WinRT
 * pages stay resident in RAM while idle.
 * ------------------------------------------------------------------------- */
typedef HANDLE HSTRING;

typedef struct IClipboardStatics2 IClipboardStatics2;

typedef struct IClipboardStatics2Vtbl {
    /* IUnknown */
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IClipboardStatics2* This, REFIID riid, void** ppvObject);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IClipboardStatics2* This);
    ULONG   (STDMETHODCALLTYPE *Release)(IClipboardStatics2* This);
    /* IInspectable */
    HRESULT (STDMETHODCALLTYPE *GetIids)(IClipboardStatics2* This, ULONG* iidCount, IID** iids);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(IClipboardStatics2* This, HSTRING* className);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(IClipboardStatics2* This, int* trustLevel);
    /* IClipboardStatics2 */
    HRESULT (STDMETHODCALLTYPE *GetHistoryItemsAsync)(IClipboardStatics2* This, void** operation);
    HRESULT (STDMETHODCALLTYPE *ClearHistory)(IClipboardStatics2* This, unsigned char* result);
} IClipboardStatics2Vtbl;

struct IClipboardStatics2 {
    const IClipboardStatics2Vtbl* lpVtbl;
};

static const GUID IID_IClipboardStatics2 = {
    0xd2ac1b6a, 0xd29f, 0x554b, { 0xb3, 0x03, 0xf0, 0x45, 0x23, 0x45, 0xfe, 0x02 }
};

typedef HRESULT (WINAPI *PFN_RoInitialize)(int initType);
typedef void    (WINAPI *PFN_RoUninitialize)(void);
typedef HRESULT (WINAPI *PFN_WindowsCreateString)(LPCWSTR sourceString, UINT32 length, HSTRING* string);
typedef HRESULT (WINAPI *PFN_WindowsDeleteString)(HSTRING string);
typedef HRESULT (WINAPI *PFN_RoGetActivationFactory)(HSTRING activatableClassId, REFIID iid, void** factory);

static void ClearWinRTClipboardHistory(void)
{
    HMODULE hComBase = LoadLibraryExW(L"combase.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!hComBase) return;

    PFN_RoInitialize pRoInit =
        (PFN_RoInitialize)GetProcAddress(hComBase, "RoInitialize");
    PFN_RoUninitialize pRoUninit =
        (PFN_RoUninitialize)GetProcAddress(hComBase, "RoUninitialize");
    PFN_WindowsCreateString pCreateStr =
        (PFN_WindowsCreateString)GetProcAddress(hComBase, "WindowsCreateString");
    PFN_WindowsDeleteString pDeleteStr =
        (PFN_WindowsDeleteString)GetProcAddress(hComBase, "WindowsDeleteString");
    PFN_RoGetActivationFactory pGetFactory =
        (PFN_RoGetActivationFactory)GetProcAddress(hComBase, "RoGetActivationFactory");

    if (pRoInit && pRoUninit && pCreateStr && pDeleteStr && pGetFactory) {
        /* 0 = RO_INIT_SINGLETHREADED (STA required for Clipboard APIs) */
        HRESULT hrInit = pRoInit(0);
        BOOL shouldUninit = SUCCEEDED(hrInit);

        if (SUCCEEDED(hrInit) || hrInit == (HRESULT)0x80010106L /* RPC_E_CHANGED_MODE */) {
            static const WCHAR kClassName[] = L"Windows.ApplicationModel.DataTransfer.Clipboard";
            HSTRING hClassId = NULL;
            if (SUCCEEDED(pCreateStr(kClassName, StrLenW(kClassName), &hClassId)) && hClassId) {
                IClipboardStatics2* pStatics2 = NULL;
                if (SUCCEEDED(pGetFactory(hClassId, &IID_IClipboardStatics2, (void**)&pStatics2)) && pStatics2) {
                    unsigned char cleared = 0;
                    pStatics2->lpVtbl->ClearHistory(pStatics2, &cleared);
                    pStatics2->lpVtbl->Release(pStatics2);
                }
                pDeleteStr(hClassId);
            }
        }

        if (shouldUninit) {
            pRoUninit();
        }
    }

    FreeLibrary(hComBase);
}

static void EmptyWin32ClipboardWithRetry(HWND hwnd)
{
    /* Retry briefly in case another application holds the clipboard lock */
    for (int attempt = 0; attempt < 15; ++attempt) {
        if (OpenClipboard(hwnd)) {
            EmptyClipboard();
            CloseClipboard();
            return;
        }
        Sleep(20);
    }
}

static void TrimProcessMemory(void)
{
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

static void ClearAllClipboardData(HWND hwnd)
{
    /* 1. Clear current clipboard (Ctrl+V: text, images, files, custom formats) */
    EmptyWin32ClipboardWithRetry(hwnd);

    /* 2. Clear Windows 10/11 Clipboard History (Win+V) */
    ClearWinRTClipboardHistory();

    /* 3. Ensure active clipboard remains empty after history purge */
    EmptyWin32ClipboardWithRetry(hwnd);

    /* 4. Immediately release temporary OS pages back to Windows */
    TrimProcessMemory();
}

/* -------------------------------------------------------------------------
 * Window Procedure for Message-Only Session Listener
 * ------------------------------------------------------------------------- */
static LRESULT CALLBACK SessionWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_WTSSESSION_CHANGE:
        if (wParam == WTS_SESSION_LOCK) {
            ClearAllClipboardData(hwnd);
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        WTSUnRegisterSessionNotification(hwnd);
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

static HWND CreateHiddenMessageWindow(HINSTANCE hInstance)
{
    WNDCLASSEXW wc;
    ZeroMem(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = SessionWndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = WINDOW_CLASS_NAME;

    RegisterClassExW(&wc);

    return CreateWindowExW(
        0,
        WINDOW_CLASS_NAME,
        WINDOW_TITLE,
        0,
        0, 0, 0, 0,
        HWND_MESSAGE, /* Message-only window: completely invisible, zero GDI/UI overhead */
        NULL,
        hInstance,
        NULL
    );
}

/* -------------------------------------------------------------------------
 * Install / Uninstall / Stop Helpers (No Admin Rights Required)
 * ------------------------------------------------------------------------- */
static BOOL StopRunningInstance(void)
{
    HWND existing = FindWindowExW(HWND_MESSAGE, NULL, WINDOW_CLASS_NAME, WINDOW_TITLE);
    if (!existing) return FALSE;

    SendMessageTimeoutW(existing, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 2000, NULL);
    for (int i = 0; i < 25; ++i) {
        if (!FindWindowExW(HWND_MESSAGE, NULL, WINDOW_CLASS_NAME, WINDOW_TITLE)) {
            return TRUE;
        }
        Sleep(40);
    }
    return TRUE;
}

static int InstallStartup(void)
{
    WCHAR exePath[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH - 3) {
        MessageBoxW(NULL, L"Impossibile determinare il percorso dell'eseguibile.", APP_NAME, MB_ICONERROR | MB_OK);
        return 1;
    }

    /* Quote path safely: "C:\path\to\CleanClipboardOnLock.exe" */
    WCHAR quotedPath[MAX_PATH + 4];
    quotedPath[0] = L'"';
    for (DWORD i = 0; i < len; ++i) {
        quotedPath[i + 1] = exePath[i];
    }
    quotedPath[len + 1] = L'"';
    quotedPath[len + 2] = L'\0';

    HKEY hKey = NULL;
    LSTATUS st = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        RUN_REG_KEY,
        0, NULL, 0,
        KEY_SET_VALUE,
        NULL,
        &hKey,
        NULL
    );
    if (st != ERROR_SUCCESS) {
        MessageBoxW(NULL, L"Errore durante l'apertura del Registro di sistema (HKCU\\Run).", APP_NAME, MB_ICONERROR | MB_OK);
        return 1;
    }

    DWORD byteSize = (StrLenW(quotedPath) + 1) * sizeof(WCHAR);
    st = RegSetValueExW(hKey, APP_NAME, 0, REG_SZ, (const BYTE*)quotedPath, byteSize);
    RegCloseKey(hKey);

    if (st != ERROR_SUCCESS) {
        MessageBoxW(NULL, L"Errore durante il salvataggio dell'avvio automatico.", APP_NAME, MB_ICONERROR | MB_OK);
        return 1;
    }

    /* Restart / launch background listener immediately */
    StopRunningInstance();

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMem(&si, sizeof(si));
    ZeroMem(&pi, sizeof(pi));
    si.cb = sizeof(si);

    if (CreateProcessW(exePath, quotedPath, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }

    MessageBoxW(
        NULL,
        L"Installazione completata!\n\n"
        L"- CleanClipboardOnLock e' ora attivo in background (invisibile).\n"
        L"- Si avviera' automaticamente ad ogni accesso a Windows.\n"
        L"- Al blocco del PC (Win+L) svuotera' Clipboard e Cronologia (Win+V).",
        APP_NAME,
        MB_ICONINFORMATION | MB_OK
    );
    return 0;
}

static int UninstallStartup(void)
{
    StopRunningInstance();

    HKEY hKey = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_REG_KEY, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegDeleteValueW(hKey, APP_NAME);
        RegCloseKey(hKey);
    }

    MessageBoxW(
        NULL,
        L"Disinstallazione completata.\n\n"
        L"Il processo in background e' stato terminato e l'avvio automatico rimosso.",
        APP_NAME,
        MB_ICONINFORMATION | MB_OK
    );
    return 0;
}

static void ShowHelpDialog(void)
{
    MessageBoxW(
        NULL,
        L"CleanClipboardOnLock - Pulizia Clipboard al blocco sessione\n\n"
        L"Opzioni da riga di comando:\n"
        L"  (nessuna)     Avvia il demone invisibile in background\n"
        L"  --install     Attiva l'avvio automatico con Windows e avvia subito\n"
        L"  --uninstall   Termina il processo e rimuove l'avvio automatico\n"
        L"  --stop        Termina silenziosamente il processo attivo\n"
        L"  --once        Svuota subito clipboard + cronologia (Win+V) ed esce\n"
        L"  --help        Mostra questa guida",
        APP_NAME,
        MB_ICONINFORMATION | MB_OK
    );
}

static UINT RunApp(void)
{
    LPCWSTR cmdLine = GetCommandLineW();

    if (HasCliFlag(cmdLine, L"--help") || HasCliFlag(cmdLine, L"-h") || HasCliFlag(cmdLine, L"/?")) {
        ShowHelpDialog();
        return 0;
    }

    if (HasCliFlag(cmdLine, L"--install")) {
        return (UINT)InstallStartup();
    }

    if (HasCliFlag(cmdLine, L"--uninstall")) {
        return (UINT)UninstallStartup();
    }

    if (HasCliFlag(cmdLine, L"--stop")) {
        StopRunningInstance();
        return 0;
    }

    HINSTANCE hInstance = GetModuleHandleW(NULL);

    /* One-shot mode (e.g. for Windows Task Scheduler on Session Lock) */
    if (HasCliFlag(cmdLine, L"--once")) {
        HWND hwnd = CreateHiddenMessageWindow(hInstance);
        ClearAllClipboardData(hwnd);
        if (hwnd) {
            DestroyWindow(hwnd);
        }
        return 0;
    }

    /* Default mode: Single-instance invisible background listener */
    HANDLE hMutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    if (!hMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    HWND hwnd = CreateHiddenMessageWindow(hInstance);
    if (!hwnd) {
        CloseHandle(hMutex);
        return 1;
    }

    if (!WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION)) {
        DestroyWindow(hwnd);
        CloseHandle(hMutex);
        return 1;
    }

    /* Minimize initial RAM footprint before sleeping in kernel message wait */
    TrimProcessMemory();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CloseHandle(hMutex);
    return (UINT)msg.wParam;
}

/* Custom CRT-free entry point */
void __stdcall WinMainCRTStartup(void)
{
    ExitProcess(RunApp());
}
