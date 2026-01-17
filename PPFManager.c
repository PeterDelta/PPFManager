#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <uxtheme.h>
#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif
#include <stdbool.h>
#include <locale.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

/* Set PPFMANAGER_DEBUG to 1 to enable developer debug logging (writes translate_debug.txt) */
#ifndef PPFMANAGER_DEBUG
#define PPFMANAGER_DEBUG 0
#endif

#if PPFMANAGER_DEBUG
static void DebugLogTranslate(const wchar_t *orig, const wchar_t *out) {
    FILE *df = _wfopen(L"translate_debug.txt", L"ab");
    if (df) {
        fwprintf(df, L"ORIG: %ls\n", orig);
        fwprintf(df, L"OUT : %ls\n\n", out);
        fclose(df);
    }
}
#else
static inline void DebugLogTranslate(const wchar_t *orig, const wchar_t *out) { (void)orig; (void)out; }
#endif

/* Global icon handles (managed by LoadAndSetIconsForDPI) */
static HICON g_hIconBig = NULL;
static HICON g_hIconSmall = NULL;

/* Get system DPI (use GetDpiForSystem when available) */
static int GetSystemDPI(void) {
    typedef UINT (WINAPI *GetDpiForSystem_t)(void);
    GetDpiForSystem_t pGetDpiForSystem = (GetDpiForSystem_t)GetProcAddress(GetModuleHandleW(L"user32"), "GetDpiForSystem");
    if (pGetDpiForSystem) return (int)pGetDpiForSystem();
    HDC hdc = GetDC(NULL);
    int dpi = 96;
    if (hdc) { dpi = GetDeviceCaps(hdc, LOGPIXELSX); ReleaseDC(NULL, hdc); }
    return dpi;
}

/* Get DPI for a window if available, fallback to device caps */
static int GetWindowDPI(HWND hwnd) {
    typedef UINT (WINAPI *GetDpiForWindow_t)(HWND);
    GetDpiForWindow_t pGetDpiForWindow = (GetDpiForWindow_t)GetProcAddress(GetModuleHandleW(L"user32"), "GetDpiForWindow");
    if (pGetDpiForWindow) return (int)pGetDpiForWindow(hwnd);
    // Fallback to system metrics
    HDC hdc = GetDC(hwnd);
    int dpi = 96;
    if (hdc) { dpi = GetDeviceCaps(hdc, LOGPIXELSX); ReleaseDC(hwnd, hdc); }
    return dpi;
}

/* Scale a base pixel value (assumed at 96 DPI) for the given window's DPI */
static int ScaleForWindow(HWND hwnd, int basePx) {
    int dpi = GetWindowDPI(hwnd);
    return MulDiv(basePx, dpi, 96);
}

/* Wrapper for GetSystemMetricsForDpi when available */
/* Forward declaration for font enum helper */
static BOOL CALLBACK SetFontEnumProc(HWND hwndChild, LPARAM lParam);

/* Attempt to load an icon from an external resources directory if present */
static BOOL TryLoadIconFromResourcesFile(LPCWSTR filename, int cx, int cy, HICON *out) {
    if (!filename || !out) return FALSE;
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(NULL, path, (DWORD)MAX_PATH)) return FALSE;
    wchar_t *p = wcsrchr(path, L'\\');
    if (p) *(p + 1) = L'\0'; else wcscpy_s(path, MAX_PATH, L".");
    // Compose resources\filename
    size_t need = wcslen(path) + wcslen(L"resources\\") + wcslen(filename) + 1;
    if (need >= MAX_PATH) return FALSE;
    wcscat_s(path, MAX_PATH, L"resources\\");
    wcscat_s(path, MAX_PATH, filename);

    HICON hIcon = (HICON)LoadImageW(NULL, path, IMAGE_ICON, cx, cy, LR_LOADFROMFILE | LR_DEFAULTCOLOR);
    if (hIcon) { *out = hIcon; return TRUE; }
    hIcon = (HICON)LoadImageW(NULL, path, IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTCOLOR | LR_DEFAULTSIZE);
    if (hIcon) { *out = hIcon; return TRUE; }
    return FALSE;
}

/* Load icon using LoadIconWithScaleDown if available (preserves alpha when scaling down)
   Extended: if the embedded resource is not found, try loading from resources/PPFManager.ico */
static HICON LoadIconWithScaleDownIfAvailable(HINSTANCE hInst, LPCWSTR name, int cx, int cy) {
    typedef HRESULT (WINAPI *LoadIconWithScaleDown_t)(HINSTANCE, PCWSTR, int, int, HICON*);
    LoadIconWithScaleDown_t pLoadIconWithScaleDown = (LoadIconWithScaleDown_t)GetProcAddress(GetModuleHandleW(L"user32"), "LoadIconWithScaleDown");
    HICON hIcon = NULL;
    if (pLoadIconWithScaleDown) {
        if (SUCCEEDED(pLoadIconWithScaleDown(hInst, name, cx, cy, &hIcon))) return hIcon;
    }
    // Fallback: try LoadImage with exact size, then default sizes (resource-based)
    hIcon = (HICON)LoadImageW(hInst, name, IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR);
    if (hIcon) return hIcon;
    hIcon = (HICON)LoadImageW(hInst, name, IMAGE_ICON, 0, 0, LR_DEFAULTCOLOR | LR_DEFAULTSIZE);
    if (hIcon) return hIcon;

    // If the icon is an integer resource (typical case: MAKEINTRESOURCEW(101)), try to load external file
    if (IS_INTRESOURCE(name)) {
        if (TryLoadIconFromResourcesFile(L"PPFManager.ico", cx, cy, &hIcon)) return hIcon;
        // Also try a fallback filename with lowercase or different name
        if (TryLoadIconFromResourcesFile(L"ppfmanager.ico", cx, cy, &hIcon)) return hIcon;
    } else {
        // If name is a string, try using it as a filename under resources/
        if (TryLoadIconFromResourcesFile(name, cx, cy, &hIcon)) return hIcon;
    }

    return NULL;
}

/* Load icon resource at sizes scaled to `dpi` and set them on the window. Destroys previous icons if present. */
static void LoadAndSetIconsForDPI(HWND hwnd, int dpi) {
    HINSTANCE hInst = GetModuleHandleW(NULL);
    int bigSize = GetSystemMetricsForDpi(SM_CXICON, dpi);
    int smallSize = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    if (g_hIconBig) { DestroyIcon(g_hIconBig); g_hIconBig = NULL; }
    if (g_hIconSmall) { DestroyIcon(g_hIconSmall); g_hIconSmall = NULL; }

    g_hIconBig = LoadIconWithScaleDownIfAvailable(hInst, MAKEINTRESOURCEW(101), bigSize, bigSize);
    if (!g_hIconBig) g_hIconBig = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON, bigSize, bigSize, LR_DEFAULTCOLOR | LR_DEFAULTSIZE);

    g_hIconSmall = LoadIconWithScaleDownIfAvailable(hInst, MAKEINTRESOURCEW(101), smallSize, smallSize);
    if (!g_hIconSmall) g_hIconSmall = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON, smallSize, smallSize, LR_DEFAULTCOLOR | LR_DEFAULTSIZE);

    /* Diagnostic: if icons are missing, emit debug info so packagers/testers saben si falta el recurso */
    if (!g_hIconBig || !g_hIconSmall) {
#if PPFMANAGER_DEBUG
        fwprintf(stderr, L"[DEBUG] LoadAndSetIconsForDPI: g_hIconBig=%p g_hIconSmall=%p\n", g_hIconBig, g_hIconSmall);
#endif
    }

    if (g_hIconBig) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)g_hIconBig);
        /* Also set class big icon so new windows get it */
        SetClassLongPtrW(hwnd, GCLP_HICON, (LONG_PTR)g_hIconBig);
    }
    if (g_hIconSmall) {
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g_hIconSmall);
        SetClassLongPtrW(hwnd, GCLP_HICONSM, (LONG_PTR)g_hIconSmall);
    }
}

#define PPFMANAGER_IMPLEMENTATION
#include "ppfmanager.h"

/* Shared helpers (moved here to centralize implementations and avoid helpers.* files) */
int safe_write(int fd, const void *buf, size_t count) {
    size_t written = 0;
    const unsigned char *p = (const unsigned char*)buf;
    while (written < count) {
        size_t remaining = count - written;
        unsigned int chunk = (remaining > (size_t)UINT_MAX) ? UINT_MAX : (unsigned int)remaining;
        int rv = _write(fd, p + written, chunk);
        if (rv < 0) return -1;
        if ((size_t)rv != chunk) return -1;
        written += rv;
    }
    return 0;
}

int safe_read(int fd, void *buf, size_t count) {
    size_t read_bytes = 0;
    unsigned char *p = (unsigned char*)buf;
    while (read_bytes < count) {
        unsigned int chunk = (count - read_bytes) > (size_t)UINT_MAX ? UINT_MAX : (unsigned int)(count - read_bytes);
        int rv = _read(fd, p + read_bytes, chunk);
        if (rv < 0) return -1;
        if (rv == 0) return -1; /* unexpected EOF */
        read_bytes += (size_t)rv;
    }
    return 0;
}

int read_le64(int fd, unsigned long long *out) {
    unsigned char buf[8];
    if (safe_read(fd, buf, 8) != 0) return -1;
    unsigned long long v = 0;
    for (int i = 0; i < 8; ++i) v |= ((unsigned long long)buf[i]) << (i*8);
    *out = v;
    return 0;
}

int read_le16(int fd, unsigned short *out) {
    unsigned char buf[2];
    if (safe_read(fd, buf, 2) != 0) return -1;
    unsigned short v = (unsigned short)buf[0] | ((unsigned short)buf[1] << 8);
    *out = v;
    return 0;
}

int PromptYesNo(const char *prompt, int defaultYes) {
    int result = defaultYes ? 1 : 0;
    /* Prefer explicit environment override for non-interactive automation: if PPFMANAGER_AUTO_YES is set,
       honor it regardless of whether stdin appears to be a TTY to avoid blocking prompts in CI/tests. */
    char *env = getenv("PPFMANAGER_AUTO_YES");
    if (env && (_stricmp(env, "1") == 0 || _stricmp(env, "true") == 0)) return 1;

    if (_isatty(_fileno(stdin))) {
        int c;
        printf("%s", prompt); fflush(stdout);
        c = getchar();
        if (c == EOF) return result;
        return (c == 'y' || c == 'Y');
    } else {
        return result;
    }
}

static wchar_t *ConvertToWidePreferUtf8ThenAcp(const char *s) {
    if (!s) return NULL;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = NULL;
    if (wlen > 0) {
        w = (wchar_t*)malloc(wlen * sizeof(wchar_t));
        if (w) {
            if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, wlen) == 0) { free(w); w = NULL; }
            else {
                for (int i = 0; i < wlen && w[i]; ++i) if (w[i] == 0xFFFD) { free(w); w = NULL; break; }
            }
        }
    }
    if (!w) {
        wlen = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
        if (wlen > 0) {
            w = (wchar_t*)malloc(wlen * sizeof(wchar_t));
            if (w) {
                if (MultiByteToWideChar(CP_ACP, 0, s, -1, w, wlen) == 0) { free(w); w = NULL; }
            }
        }
    }
    return w;
}

void PrintDescriptionBytes(const unsigned char *desc) {
    if (!desc) { printf("Description : \n"); return; }
    unsigned char desc_trimmed[51];
    memcpy(desc_trimmed, desc, 50);
    desc_trimmed[50] = 0;
    int len = 50; while (len > 0 && desc_trimmed[len-1] == ' ') { desc_trimmed[--len] = 0; }
    const char *to_print = (const char*)desc_trimmed;

    wchar_t *w = ConvertToWidePreferUtf8ThenAcp(to_print);
    if (w) {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD written = 0;
        if (hOut && hOut != INVALID_HANDLE_VALUE) {
            DWORD mode;
            if (GetConsoleMode(hOut, &mode)) {
                WriteConsoleW(hOut, L"Description : ", (DWORD)wcslen(L"Description : "), &written, NULL);
                WriteConsoleW(hOut, w, (DWORD)wcslen(w), &written, NULL);
                WriteConsoleW(hOut, L"\n", 1, &written, NULL);
                free(w);
                return;
            }
        }
        int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
        if (need > 0) {
            char outbuf[1024];
            char *out = NULL;
            if ((size_t)need <= sizeof(outbuf)) out = outbuf; else out = (char*)malloc(need);
            if (!out) { free(w); printf("Description : %s\n", to_print); return; }
            WideCharToMultiByte(CP_UTF8, 0, w, -1, out, need, NULL, NULL);
            printf("Description : %s\n", out);
            if ((size_t)need > sizeof(outbuf)) free(out);
        } else {
            printf("Description : %s\n", to_print);
        }
        free(w);
        return;
    }
    printf("Description : %s\n", to_print);
}

void PrintRawTextBytes(const unsigned char *s) {
    if (!s) { printf("\n"); return; }
    wchar_t *w = ConvertToWidePreferUtf8ThenAcp((const char*)s);
    if (w) {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD written = 0, mode;
        if (hOut && hOut != INVALID_HANDLE_VALUE && GetConsoleMode(hOut, &mode)) {
            WriteConsoleW(hOut, w, (DWORD)wcslen(w), &written, NULL);
            WriteConsoleW(hOut, L"\n", 1, &written, NULL);
            free(w);
            return;
        }
        int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
        if (need > 0) {
            char outbuf[1024];
            char *out = NULL;
            if ((size_t)need <= sizeof(outbuf)) out = outbuf; else out = (char*)malloc(need);
            if (!out) { free(w); printf("%s\n", s); return; }
            WideCharToMultiByte(CP_UTF8, 0, w, -1, out, need, NULL, NULL);
            printf("%s\n", out);
            if ((size_t)need > sizeof(outbuf)) free(out);
            free(w);
            return;
        }
        free(w);
    }
    printf("%s\n", s);
}



void PrintWin32ErrorFmt(const char *fmt, ...) {
    char prefix[512];
    va_list ap; va_start(ap, fmt); _vsnprintf_s(prefix, sizeof(prefix), _TRUNCATE, fmt, ap); va_end(ap);
    prefix[sizeof(prefix)-1] = '\0';
    DWORD err = GetLastError();
    char msg[512] = {0};
    if (FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err,
                       MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), msg, sizeof(msg), NULL) > 0) {
        size_t n = strlen(msg); while (n > 0 && (msg[n-1] == '\r' || msg[n-1] == '\n')) msg[--n] = '\0';
        fprintf(stderr, "%s: %s (GetLastError=%lu)\n", prefix, msg, (unsigned long)err);
    } else {
        fprintf(stderr, "%s: GetLastError=%lu\n", prefix, (unsigned long)err);
    }
}

/* Explicit little-endian integer read/write helpers for PPF wire format */
int write_le64(int fd, unsigned long long val) {
    unsigned char buf[8];
    for (int i = 0; i < 8; ++i) buf[i] = (unsigned char)((val >> (i*8)) & 0xFF);
    return safe_write(fd, buf, 8);
}
int write_le16(int fd, unsigned short val) {
    unsigned char buf[2];
    buf[0] = (unsigned char)(val & 0xFF);
    buf[1] = (unsigned char)((val >> 8) & 0xFF);
    return safe_write(fd, buf, 2);
}
/* read_le64/read_le16 implemented above using safe_read to avoid partial reads */

// Helper: convert old-style filter string ("Name\0pattern\0...") into COMDLG_FILTERSPEC
static COMDLG_FILTERSPEC *ParseFilterSpec(const wchar_t *filter, UINT *outCount) {
    if (!filter) { *outCount = 0; return NULL; }
    // Count pairs
    const wchar_t *p = filter;
    UINT count = 0;
    while (*p) {
        size_t n = wcslen(p);
        p += n + 1;
        if (*p == 0) break;
        size_t m = wcslen(p);
        p += m + 1;
        count++;
    }
    if (count == 0) { *outCount = 0; return NULL; }
    COMDLG_FILTERSPEC *specs = (COMDLG_FILTERSPEC*)CoTaskMemAlloc(sizeof(COMDLG_FILTERSPEC) * count);
    if (!specs) { *outCount = 0; return NULL; }
    p = filter; UINT i = 0;
    while (*p && i < count) {
        specs[i].pszName = p;
        p += wcslen(p) + 1;
        specs[i].pszSpec = p;
        p += wcslen(p) + 1;
        i++;
    }
    *outCount = count;
    return specs;
}

// Show File dialog using IFileSaveDialog with initial folder; return TRUE and filename in outFilename on success
static BOOL ShowSaveFileDialog_COM(HWND owner, wchar_t *outFilename, size_t outSize, const wchar_t *initialDir, const wchar_t *filter, HRESULT *outHr, DWORD flags) {
    if (outHr) *outHr = S_OK;
    HRESULT hrInit = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (outHr) *outHr = hrInit;
    if (FAILED(hrInit)) return FALSE;
    IFileOpenDialog *pfd = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void**)&pfd);
    if (outHr) *outHr = hr;
    if (FAILED(hr)) { CoUninitialize(); return FALSE; }
    if (flags & OFN_FILEMUSTEXIST) {
        pfd->lpVtbl->SetOptions(pfd, FOS_FILEMUSTEXIST);
    }
    if (initialDir && initialDir[0]) {
        IShellItem *psiFolder = NULL;
        hr = SHCreateItemFromParsingName(initialDir, NULL, &IID_IShellItem, (void**)&psiFolder);
        if (SUCCEEDED(hr) && psiFolder) {
            pfd->lpVtbl->SetFolder(pfd, psiFolder);
            psiFolder->lpVtbl->Release(psiFolder);
        }
    }
    UINT count = 0; COMDLG_FILTERSPEC *specs = ParseFilterSpec(filter, &count);
    if (specs && count > 0) {
        pfd->lpVtbl->SetFileTypes(pfd, count, specs);
        CoTaskMemFree(specs);
    }
    // Disable overwrite prompt - silently overwrite files
    DWORD dwFlags;
    if (SUCCEEDED(pfd->lpVtbl->GetOptions(pfd, &dwFlags))) {
        pfd->lpVtbl->SetOptions(pfd, dwFlags & ~FOS_OVERWRITEPROMPT);
    }
    hr = pfd->lpVtbl->Show(pfd, owner);
    if (outHr) *outHr = hr;
    if (SUCCEEDED(hr)) {
        IShellItem *psi = NULL;
        hr = pfd->lpVtbl->GetResult(pfd, &psi);
        if (SUCCEEDED(hr) && psi) {
            PWSTR pszPath = NULL;
            hr = psi->lpVtbl->GetDisplayName(psi, SIGDN_FILESYSPATH, &pszPath);
            if (SUCCEEDED(hr) && pszPath) {
                wcsncpy(outFilename, pszPath, outSize - 1);
                outFilename[outSize - 1] = 0;
                CoTaskMemFree(pszPath);
                psi->lpVtbl->Release(psi);
                pfd->lpVtbl->Release(pfd);
                CoUninitialize();
                return TRUE;
            }
            psi->lpVtbl->Release(psi);
        }
    }
    pfd->lpVtbl->Release(pfd);
    CoUninitialize();
    return FALSE;
}

// Show Open dialog using IFileOpenDialog with initial folder; return TRUE and filename in outFilename on success
// (Removed, now using unified ShowSaveFileDialog_COM)

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
// Tab control color messages (not always present depending on headers)
#ifndef TCM_SETBKCOLOR
#define TCM_SETBKCOLOR (TCM_FIRST + 67)
#endif
#ifndef TCM_SETTEXTCOLOR
#define TCM_SETTEXTCOLOR (TCM_FIRST + 68)
#endif
#ifndef CCM_SETBKCOLOR
#define CCM_SETBKCOLOR 0x2001
#endif

static int g_console_attached = 0;

#define WM_APPEND_OUTPUT (WM_APP + 100)
#define WM_ENABLE_BROWSE (WM_APP + 101)  /* wParam: 1 enable, 0 disable */
#define WM_CREAR_PROGRESS (WM_APP + 102) /* wParam: progress 0..10000 */
#define WM_APLICAR_PROGRESS (WM_APP + 103) /* wParam: progress 0..10000 */

// Idioma actual de la UI: 0=ES, 1=EN
enum { LANG_ES = 0, LANG_EN = 1 };
static int g_lang = LANG_ES;
static int g_crearValidUserSet = 0; /* 1 = user manually changed validation checkbox */

typedef struct {
    HWND hEdit;
    wchar_t cmdline[1024];
    char partial[4096];
    int partial_len;
} PROC_THREAD_PARAM;

// Common styles so every button/checkbox uses the native Windows look and keyboard focus
static const DWORD BTN_STYLE = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;
static const DWORD BTN_STYLE_DEFAULT = BTN_STYLE | BS_DEFPUSHBUTTON;
static const DWORD CHK_STYLE = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX;
static int g_themePref = 0; // 0=claro, 1=oscuro
static bool g_isDark = false;
static COLORREF g_clrBg = 0, g_clrText = 0, g_clrEditBg = 0, g_clrEditText = 0, g_clrBorder = 0;
static COLORREF g_clrTabSel = 0, g_clrTabNorm = 0, g_clrTabTextSel = 0, g_clrTabTextNorm = 0;
// Tab hover / selection animation state
static int g_tabHover = -1;            // index currently under mouse
static int g_tabLastSelected = -1;     // last selected index
static int g_tabAnimFrom = -1;         // animating from index
static int g_tabAnimTo = -1;           // animating to index
static int g_tabAnimStep = 0;          // current animation step
static const int TAB_ANIM_STEPS = 6;   // steps in fade animation
static const int TAB_ANIM_INTERVAL = 30; // ms per step
static const UINT ID_TAB_ANIM_TIMER = 0x501; // timer id for tab animation
static HBRUSH g_brBg = NULL, g_brEditBg = NULL;
static HFONT hFont = NULL;
/* Shared globals used by MakePPF and ApplyPPF when building combined GUI */
int ppf = 0, bin = 0, mod = 0, fileid = 0;
char binblock[1024];
char ppfblock[1024];
unsigned char ppfmem[512];
char temp_ppfname[512] = {0};
int using_temp = 0;
int patch_ok = 0;
static volatile LONG g_operation_running = 0; // Flag to prevent concurrent operations

/* Forward declarations for functions from ApplyPPF.c and MakePPF.c */
extern int ApplyPPF_Main(int argc, char **argv);
extern int MakePPF_Main(int argc, char **argv);
extern void MakePPF_InitArgs(void);
extern void MakePPF_SetProgressCallback(void (*cb)(double));
extern void ApplyPPF_SetProgressCallback(void (*cb)(double));

/* Reset all global variables before each operation to prevent state corruption */
static void ResetGlobalState(void) {
    ppf = 0;
    bin = 0;
    mod = 0;
    fileid = 0;
    using_temp = 0;
    patch_ok = 0;
    temp_ppfname[0] = '\0';
}

// Apply default theme; passing NULL restores class theme (empty strings disable it)
static void ApplyTheme(HWND h) { if (h) SetWindowTheme(h, NULL, NULL); }

// Windows 11 border color for edit controls
static void ApplyEditBorderColor(HWND hEdit, bool dark) {
    if (!hEdit) return;
    // Set border color via WM_NCPAINT for Windows 11 style
    HMODULE hDwm = LoadLibraryW(L"dwmapi.dll");
    if (hDwm) {
        typedef HRESULT(WINAPI *PFNDwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
        PFNDwmSetWindowAttribute p = (PFNDwmSetWindowAttribute)GetProcAddress(hDwm, "DwmSetWindowAttribute");
        if (p) {
            COLORREF crBorder = dark ? RGB(60, 60, 60) : RGB(200, 200, 200);
            p(hEdit, DWMWA_BORDER_COLOR, &crBorder, sizeof(crBorder));
        }
        FreeLibrary(hDwm);
    }
}

static void SetTitleBarDark(HWND hwnd, bool enable) {
    HMODULE hDwm = LoadLibraryW(L"dwmapi.dll");
    if (!hDwm) return;
    typedef HRESULT(WINAPI *PFNDwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
    PFNDwmSetWindowAttribute p = (PFNDwmSetWindowAttribute)GetProcAddress(hDwm, "DwmSetWindowAttribute");
    if (p) {
        BOOL useDark = enable ? TRUE : FALSE;
        p(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark, sizeof(useDark));
    }
    FreeLibrary(hDwm);
}

static void UpdateCheckboxThemes(bool dark, HWND chkUndo, HWND chkValid, HWND chkRevert) {
    const wchar_t *theme = dark ? L"DarkMode_Explorer" : NULL;
    if (chkUndo) SetWindowTheme(chkUndo, theme, NULL);
    if (chkValid) SetWindowTheme(chkValid, theme, NULL);
    if (chkRevert) SetWindowTheme(chkRevert, theme, NULL);
}

static void UpdateButtonThemes(bool dark, HWND *btns, int count) {
    const wchar_t *theme = dark ? L"DarkMode_Explorer" : NULL;
    for (int i = 0; i < count; ++i) {
        if (btns[i]) SetWindowTheme(btns[i], theme, NULL);
    }
}

static void UpdateControlThemes(bool dark, HWND *controls, int count) {
    const wchar_t *theme = dark ? L"DarkMode_Explorer" : NULL;
    for (int i = 0; i < count; ++i) {
        if (controls[i]) {
            SetWindowTheme(controls[i], theme, NULL);
            ApplyEditBorderColor(controls[i], dark);
        }
    }
}

// Owner-draw helpers removed to keep system menu rendering consistent

static void UpdateThemeBrushes(bool dark) {
    g_isDark = dark;
    // delete previous custom brushes if any
    if (g_brBg && g_brBg != GetSysColorBrush(COLOR_BTNFACE)) { DeleteObject(g_brBg); }
    if (g_brEditBg && g_brEditBg != GetSysColorBrush(COLOR_WINDOW)) { DeleteObject(g_brEditBg); }

    if (dark) {
        g_clrBg = RGB(32, 32, 32); // fondo general - Windows 11 dark
        g_clrText = RGB(229, 229, 229); // texto general
        g_clrEditBg = RGB(43, 43, 43); // fondo campos de texto - Windows 11 dark input
        g_clrEditText = RGB(229, 229, 229); // texto campos de texto
        g_clrBorder = RGB(60, 60, 60); // borde controles
        g_clrTabSel = RGB(85, 85, 85);
        g_clrTabNorm = RGB(45, 45, 45);
        g_clrTabTextSel = RGB(245, 245, 245);
        g_clrTabTextNorm = RGB(180, 180, 180);
        g_brBg = CreateSolidBrush(g_clrBg); // pincel fondo general
        g_brEditBg = CreateSolidBrush(g_clrEditBg); // pincel fondo campos de texto
    } else {
        g_clrBg = GetSysColor(COLOR_BTNFACE);
        g_clrText = GetSysColor(COLOR_BTNTEXT);
        g_clrEditBg = GetSysColor(COLOR_WINDOW);
        g_clrEditText = GetSysColor(COLOR_WINDOWTEXT);
        g_clrBorder = RGB(200, 200, 200); // borde controles en modo claro
        g_clrTabSel = RGB(240,240,240);
        g_clrTabNorm = RGB(230,230,230);
        g_clrTabTextSel = RGB(0,0,0);
        g_clrTabTextNorm = RGB(80,80,80);
        g_brBg = GetSysColorBrush(COLOR_BTNFACE);
        g_brEditBg = GetSysColorBrush(COLOR_WINDOW);
    }
}

static COLORREF LerpColor(COLORREF a, COLORREF b, int step, int steps) {
    if (step <= 0) return a;
    if (step >= steps) return b;
    int r1 = GetRValue(a), g1 = GetGValue(a), b1 = GetBValue(a);
    int r2 = GetRValue(b), g2 = GetGValue(b), b2 = GetBValue(b);
    int r = r1 + (r2 - r1) * step / steps;
    int g = g1 + (g2 - g1) * step / steps;
    int bl = b1 + (b2 - b1) * step / steps;
    return RGB(r, g, bl);
}

static void UpdateThemeMenuChecks(HMENU hMenuBar, bool dark) {
    if (!hMenuBar) return;
    HMENU hMenuTema = GetSubMenu(hMenuBar, 1); // Tema está en posición 1
    if (!hMenuTema) return;
    CheckMenuItem(hMenuTema, 203, MF_BYCOMMAND | (dark ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hMenuTema, 204, MF_BYCOMMAND | (!dark ? MF_CHECKED : MF_UNCHECKED));
}

static void UpdateLanguageMenuChecks(HMENU hMenuBar) {
    if (!hMenuBar) return;
    HMENU hMenuIdioma = GetSubMenu(hMenuBar, 0); // Idioma está en posición 0
    if (!hMenuIdioma) return;
    CheckMenuItem(hMenuIdioma, 301, MF_BYCOMMAND | (g_lang == LANG_ES ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hMenuIdioma, 302, MF_BYCOMMAND | (g_lang == LANG_EN ? MF_CHECKED : MF_UNCHECKED));
}

static void UpdatePanelEdge(HWND panel) {
    if (!panel) return;
    LONG_PTR ex = GetWindowLongPtrW(panel, GWL_EXSTYLE);
    LONG_PTR newEx = ex & ~WS_EX_WINDOWEDGE; // siempre sin borde
    if (newEx != ex) {
        SetWindowLongPtrW(panel, GWL_EXSTYLE, newEx);
        SetWindowPos(panel, NULL, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
    }
}

static void ApplyCurrentTheme(bool dark, HWND hwnd, HWND hwndTab, HWND hCrearPanel, HWND hAplicarPanel,
    HWND chkUndo, HWND chkValid, HWND chkRevert, HMENU hMenuBar) {
    UpdateThemeBrushes(dark);
    SetTitleBarDark(hwnd, dark);
    if (hwndTab) {
        LONG style = GetWindowLongW(hwndTab, GWL_STYLE);
        if (dark) {
            style |= TCS_OWNERDRAWFIXED;
            SetWindowTheme(hwndTab, L"", L"");
            SendMessageW(hwndTab, CCM_SETBKCOLOR, 0, (LPARAM)g_clrBg);
            SendMessageW(hwndTab, TCM_SETBKCOLOR, 0, (LPARAM)g_clrBg);
            SendMessageW(hwndTab, TCM_SETTEXTCOLOR, 0, (LPARAM)g_clrText);
        } else {
            style &= ~TCS_OWNERDRAWFIXED;
            SetWindowTheme(hwndTab, NULL, NULL);
            SendMessageW(hwndTab, CCM_SETBKCOLOR, 0, (LPARAM)CLR_DEFAULT);
            SendMessageW(hwndTab, TCM_SETBKCOLOR, 0, (LPARAM)CLR_DEFAULT);
            SendMessageW(hwndTab, TCM_SETTEXTCOLOR, 0, (LPARAM)CLR_DEFAULT);
        }
        SetWindowLongW(hwndTab, GWL_STYLE, style);
        SetWindowPos(hwndTab, NULL, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
    }
    // Quitar borde 3D en paneles en modo oscuro para que no se vean claros
    UpdatePanelEdge(hCrearPanel);
    UpdatePanelEdge(hAplicarPanel);
    UpdateCheckboxThemes(dark, chkUndo, chkValid, chkRevert);
    if (hMenuBar) {
        UpdateThemeMenuChecks(hMenuBar, dark);
        UpdateLanguageMenuChecks(hMenuBar);
        DrawMenuBar(hwnd);
    }
    if (hwnd) InvalidateRect(hwnd, NULL, TRUE);
    if (hCrearPanel) InvalidateRect(hCrearPanel, NULL, TRUE);
    if (hAplicarPanel) InvalidateRect(hAplicarPanel, NULL, TRUE);
    if (hwndTab) InvalidateRect(hwndTab, NULL, TRUE);
}

// Panel subclass globals and forward declarations
static WNDPROC oldCrearPanelProc = NULL;
static WNDPROC oldAplicarPanelProc = NULL;
static WNDPROC oldTabProc = NULL;
static WNDPROC oldComboProc = NULL;
static HWND g_hCrearOutput = NULL;
static HWND g_hAplicarOutput = NULL;
static HWND g_hwndMain = NULL;
static HWND g_hCrearTopProgress = NULL; /* Global progress control handle (create-tab) */

/* Apply-tab progress control (mirrors Create-tab helpers) */
static HWND g_hAplicarTopProgress = NULL; /* Global progress control handle (apply-tab) */

/* Helper functions for create-tab progress control (file-scope to avoid nested functions) */
static void CrearProgress_SetPos(int ipct) {
    if (g_hCrearTopProgress && IsWindow(g_hCrearTopProgress)) {
        SendMessageW(g_hCrearTopProgress, PBM_SETPOS, (WPARAM)ipct, 0);
        ShowWindow(g_hCrearTopProgress, SW_SHOWNOACTIVATE);
        EnableWindow(g_hCrearTopProgress, FALSE);
    }
}
static void CrearProgress_ResetToZero(void) {
    CrearProgress_SetPos(0);
}

/* Apply-tab progress control (mirrors Create-tab helpers) */
static void AplicarProgress_SetPos(int ipct) {
    if (g_hAplicarTopProgress && IsWindow(g_hAplicarTopProgress)) {
        SendMessageW(g_hAplicarTopProgress, PBM_SETPOS, (WPARAM)ipct, 0);
        ShowWindow(g_hAplicarTopProgress, SW_SHOWNOACTIVATE);
        EnableWindow(g_hAplicarTopProgress, FALSE);
    }
}
static void AplicarProgress_ResetToZero(void) {
    AplicarProgress_SetPos(0);
}

/* Top-level GUI progress poster used by MakePPF callback. Declared here so it can use g_hwndMain. */

/* Top-level GUI progress poster used by MakePPF callback. Declared here so it can use g_hwndMain. */
static void GuiMakePPFProgress(double pct) {
    int ipct = (int)(pct * 100.0 + 0.5); /* 0..10000 */
    if (ipct < 0) ipct = 0;
    if (ipct > 10000) ipct = 10000;
    HWND tgt = g_hwndMain ? g_hwndMain : GetForegroundWindow();
    PostMessageW(tgt, WM_CREAR_PROGRESS, (WPARAM)ipct, 0);
}

/* Top-level GUI progress poster used by ApplyPPF callback. Declared here so it can use g_hwndMain. */
static void GuiApplyProgress(double pct) {
    int ipct = (int)(pct * 100.0 + 0.5); /* 0..10000 */
    if (ipct < 0) ipct = 0;
    if (ipct > 10000) ipct = 10000;
    HWND tgt = g_hwndMain ? g_hwndMain : GetForegroundWindow();
    PostMessageW(tgt, WM_APLICAR_PROGRESS, (WPARAM)ipct, 0);
}

// Globals for controls accessible from other functions
static HWND g_hwndTab = NULL;
static HWND g_hCrearPanel = NULL;
static HWND g_hAplicarPanel = NULL;
LRESULT CALLBACK CrearPanelProc(HWND hwndPanel, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK AplicarPanelProc(HWND hwndPanel, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK TabProc(HWND hwndTab, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK ComboProc(HWND hwndCombo, UINT msg, WPARAM wParam, LPARAM lParam);
/* Helper: safely post an allocated string to the UI (frees on failure) */
static BOOL SafePostAllocatedString(HWND tgt, UINT msg, WPARAM wParam, wchar_t *wstr);
static void ForceLayoutRefresh(void);
LRESULT CALLBACK AboutWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

/* Implementation: safely post an allocated, heap-allocated wide string to UI.
   On failure (invalid target or PostMessage failure), the buffer is freed to
   avoid memory leaks. */
static BOOL SafePostAllocatedString(HWND tgt, UINT msg, WPARAM wParam, wchar_t *wstr) {
    if (!wstr) return FALSE;
    HWND t = tgt ? tgt : GetForegroundWindow();
    if (!t || !IsWindow(t)) { free(wstr); return FALSE; }
    if (!PostMessageW(t, msg, wParam, (LPARAM)wstr)) { free(wstr); return FALSE; }
    return TRUE;
}


// --- TRADUCCIÓN CENTRALIZADA ---

// Forward declaration for T function
static const wchar_t* T(const wchar_t* id);

// Show Help dialog (modal, centered on parent)
static void ShowHelpDialog(HWND hwnd) {
    const wchar_t *title = T(L"help_title");
    const wchar_t *text = T(L"help_interface");

    /* Register about class once */
    static ATOM cls = 0;
    if (!cls) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = AboutWndProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"PPFManagerAboutClass";
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hIcon = LoadIconWithScaleDownIfAvailable(GetModuleHandleW(NULL), MAKEINTRESOURCEW(101),
                                                   GetSystemMetricsForDpi(SM_CXICON, GetSystemDPI()),
                                                   GetSystemMetricsForDpi(SM_CYICON, GetSystemDPI()));
        cls = RegisterClassW(&wc);
    }

    /* Fixed size scaled for DPI */
    RECT prc; GetWindowRect(hwnd, &prc);
    int pw = prc.right - prc.left;
    int dlgW = ScaleForWindow(hwnd, 700); // Ancho ventana ayuda
    int dlgH = ScaleForWindow(hwnd, 460); // Alto ventana ayuda
    {
        int screenH = GetSystemMetrics(SM_CYSCREEN);
        if (dlgH > screenH - 40) dlgH = screenH - 40;
    }

    int x = prc.left + (pw - dlgW) / 2;
    int y = prc.top + ((prc.bottom - prc.top) - dlgH) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    HWND hDlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE, L"PPFManagerAboutClass", title,
                                 WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                 x, y, dlgW, dlgH, hwnd, NULL, GetModuleHandleW(NULL), (LPVOID)text);
    if (hDlg) {
        // Put app icon on the title bar for Help
        HINSTANCE hInst = GetModuleHandleW(NULL);
        int sysdpi_ab = GetSystemDPI();
        HICON hBig = LoadIconWithScaleDownIfAvailable(hInst, MAKEINTRESOURCEW(101),
                                                      GetSystemMetricsForDpi(SM_CXICON, sysdpi_ab),
                                                      GetSystemMetricsForDpi(SM_CYICON, sysdpi_ab));
        HICON hSmall = LoadIconWithScaleDownIfAvailable(hInst, MAKEINTRESOURCEW(101),
                                                        GetSystemMetricsForDpi(SM_CXSMICON, sysdpi_ab),
                                                        GetSystemMetricsForDpi(SM_CYSMICON, sysdpi_ab));
        if (hBig) SendMessageW(hDlg, WM_SETICON, ICON_BIG, (LPARAM)hBig);
        if (hSmall) SendMessageW(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)hSmall);
    }
    if (!hDlg) {
        MessageBoxW(hwnd, text, title, MB_OK | MB_ICONINFORMATION);
        return;
    }

    EnableWindow(hwnd, FALSE);
    ShowWindow(hDlg, SW_SHOW);
    SetActiveWindow(hDlg);

    MSG msg;
    while (IsWindow(hDlg) && GetMessageW(&msg, NULL, 0, 0)) {
        if (!IsWindow(hDlg)) break;
        if (!IsDialogMessage(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(hwnd, TRUE);
    SetActiveWindow(hwnd);
}

// Show About dialog (modal, centered on parent)
static void ShowAboutDialog(HWND hwnd) {
    const wchar_t *title = L"PPF Manager";
    const wchar_t *text_es = L"\nPPF Manager vpre-1.0 por PeterDelta\r\nBasado en fuentes PPF3 de Icarus/Paradox\r\n\r\nhttps://github.com/PeterDelta/PPFManager";
    const wchar_t *text_en = L"\nPPF Manager vpre-1.0 by PeterDelta\r\nBased on PPF3 sources by Icarus/Paradox\r\n\r\nhttps://github.com/PeterDelta/PPFManager";
    const wchar_t *text = (g_lang == LANG_EN) ? text_en : text_es;

    /* Register about class once */
    static ATOM cls = 0;
    if (!cls) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = AboutWndProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"PPFManagerAboutClass";
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        /* Use system DPI for about dialog class icon */
        int sysdpi_local = GetSystemDPI();
        wc.hIcon = LoadIconWithScaleDownIfAvailable(wc.hInstance, MAKEINTRESOURCEW(101),
                                                   GetSystemMetricsForDpi(SM_CXICON, sysdpi_local),
                                                   GetSystemMetricsForDpi(SM_CYICON, sysdpi_local));
        cls = RegisterClassW(&wc);
    }

    /* Measure text for sizing */
    RECT prc; GetWindowRect(hwnd, &prc);
    int pw = prc.right - prc.left;
    int maxW = pw - 80; if (maxW < 240) maxW = 240;
    HDC hdc = GetDC(hwnd);
    HFONT hf = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HGDIOBJ oldf = NULL;
    if (hf) oldf = SelectObject(hdc, hf);
    RECT rcText = {0,0,maxW,0};
    DrawTextW(hdc, text, -1, &rcText, DT_CALCRECT | DT_WORDBREAK | DT_CENTER);
    if (oldf) SelectObject(hdc, oldf);
    ReleaseDC(hwnd, hdc);

    int padX = ScaleForWindow(hwnd,24), padY = ScaleForWindow(hwnd,18), btnH = ScaleForWindow(hwnd,32);
    int dlgW = rcText.right + padX; int minDlgW = ScaleForWindow(hwnd,300); if (dlgW < minDlgW) dlgW = minDlgW;
    int dlgH = rcText.bottom + padY + btnH + ScaleForWindow(hwnd,12); int minDlgH = ScaleForWindow(hwnd,120); if (dlgH < minDlgH) dlgH = minDlgH;
    /* Allow explicit height via env var PPFMANAGER_ABOUT_HEIGHT (pixels). If not set, default multiplier 2 */
    {
        wchar_t envbuf[32] = {0};
        if (GetEnvironmentVariableW(L"PPFMANAGER_ABOUT_HEIGHT", envbuf, 32) > 0) {
            int env_h = _wtoi(envbuf);
            if (env_h > 0) dlgH = env_h;
        } else {
            dlgH *= 1.55;
        }
    }
    {
        int screenH = GetSystemMetrics(SM_CYSCREEN);
        if (dlgH > screenH - 40) dlgH = screenH - 40;
    }

    int x = prc.left + (pw - dlgW) / 2;
    int y = prc.top + ((prc.bottom - prc.top) - dlgH) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    HWND hDlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE, L"PPFManagerAboutClass", title,
                                 WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                 x, y, dlgW, dlgH, hwnd, NULL, GetModuleHandleW(NULL), (LPVOID)text);
    if (hDlg) {
        // Put app icon on the title bar for About
        HINSTANCE hInst = GetModuleHandleW(NULL);
        int sysdpi_ab = GetSystemDPI();
        HICON hBig = LoadIconWithScaleDownIfAvailable(hInst, MAKEINTRESOURCEW(101),
                                                      GetSystemMetricsForDpi(SM_CXICON, sysdpi_ab),
                                                      GetSystemMetricsForDpi(SM_CYICON, sysdpi_ab));
        HICON hSmall = LoadIconWithScaleDownIfAvailable(hInst, MAKEINTRESOURCEW(101),
                                                        GetSystemMetricsForDpi(SM_CXSMICON, sysdpi_ab),
                                                        GetSystemMetricsForDpi(SM_CYSMICON, sysdpi_ab));
        if (hBig) SendMessageW(hDlg, WM_SETICON, ICON_BIG, (LPARAM)hBig);
        if (hSmall) SendMessageW(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)hSmall);
    }
    if (!hDlg) {
        MessageBoxW(hwnd, text, title, MB_OK);
        return;
    }

    EnableWindow(hwnd, FALSE);
    ShowWindow(hDlg, SW_SHOW);
    SetActiveWindow(hDlg);

    MSG msg;
    while (IsWindow(hDlg) && GetMessageW(&msg, NULL, 0, 0)) {
        if (!IsWindow(hDlg)) break;
        if (!IsDialogMessage(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(hwnd, TRUE);
    SetActiveWindow(hwnd);
}

// Tabla de mensajes bilingüe
static const wchar_t* tw(const char* key) {
    // Mensajes bilingües
    if (strcmp(key, "createprocess_failed") == 0)
        return (g_lang == LANG_EN)
            ? L"CreateProcess failed: %lu\r\n"
            : L"Fallo CreateProcess: %lu\r\n";
    if (strcmp(key, "getsave_failed") == 0)
        return (g_lang == LANG_EN)
            ? L"GetSaveFileNameW failed (id=%d) CommDlgExtendedError=0x%08lX"
            : L"GetSaveFileNameW falló (id=%d) CommDlgExtendedError=0x%08lX";
    if (strcmp(key, "getopen_failed") == 0)
        return (g_lang == LANG_EN)
            ? L"GetOpenFileNameW failed (id=%d) CommDlgExtendedError=0x%08lX"
            : L"GetOpenFileNameW falló (id=%d) CommDlgExtendedError=0x%08lX";
    if (strcmp(key, "exec") == 0)
        return (g_lang == LANG_EN)
            ? L"Execute: %s"
            : L"Ejecutando: %s";
    if (strcmp(key, "select_ppf_info") == 0)
        return (g_lang == LANG_EN)
            ? L"Select a PPF file to view its information.\r\n"
            : L"Selecciona un archivo PPF para ver la información.\r\n";
    if (strcmp(key, "select_ppf_fileid") == 0)
        return (g_lang == LANG_EN)
            ? L"Select a PPF and file_id.diz to add.\r\n"
            : L"Selecciona PPF y file_id.diz para añadir.\r\n";
    if (strcmp(key, "select_create_origname") == 0)
        return (g_lang == LANG_EN) ? L"Select the original image file.\r\n" : L"Selecciona la imagen original.\r\n";
    if (strcmp(key, "select_create_modname") == 0)
        return (g_lang == LANG_EN) ? L"Select the modified image file.\r\n" : L"Selecciona la imagen modificada.\r\n";
    if (strcmp(key, "select_create_ppfname") == 0)
        return (g_lang == LANG_EN) ? L"Select an output PPF filename.\r\n" : L"Selecciona el archivo PPF de salida.\r\n";
    if (strcmp(key, "select_apply_bin") == 0)
        return (g_lang == LANG_EN) ? L"Select a bin/GI/ISO image file to apply the patch to.\r\n" : L"Selecciona una imagen (BIN/GI/ISO) donde aplicar el parche.\r\n";
    if (strcmp(key, "select_apply_ppf") == 0)
        return (g_lang == LANG_EN) ? L"Select a PPF patch file to apply.\r\n" : L"Selecciona un archivo PPF para aplicar.\r\n";
    if (strcmp(key, "patch_created") == 0)
        return (g_lang == LANG_EN) ? L"Patch created successfully.\n" : L"Parche creado correctamente.\n";
    if (strcmp(key, "error_could_not_open_files_create") == 0)
        return (g_lang == LANG_EN) ? L"Error: could not open files for patch creation.\n" : L"Error: no se pudieron abrir los archivos para crear el parche.\n";
    if (strcmp(key, "usage_s") == 0)
        return (g_lang == LANG_EN) ? L"Usage: PPFManager.exe s <ppf>\n" : L"Uso: PPFManager.exe s <ppf>\n";
    if (strcmp(key, "makeppf_header") == 0)
        return (g_lang == LANG_EN) ? L"" : L"";
    if (strcmp(key, "error_cannot_open_file") == 0)
        return (g_lang == LANG_EN) ? L"Error: cannot open file '%s'\n" : L"Error: no se puede abrir el archivo '%s'\n";
    if (strcmp(key, "error_not_ppf3") == 0)
        return (g_lang == LANG_EN) ? L"Error: file '%s' is no PPF3.0 patch\n" : L"Error: el archivo '%s' no es un parche PPF3.0\n";
    if (strcmp(key, "done") == 0)
        return (g_lang == LANG_EN) ? L"Done.\n" : L"Completado.\n";
    if (strcmp(key, "usage_f_addfileid") == 0)
        return (g_lang == LANG_EN) ? L"Usage: PPFManager.exe f <ppf> <file_id.diz>\n" : L"Uso: PPFManager.exe f <ppf> <file_id.diz>\n";
    if (strcmp(key, "error_cannot_open_files") == 0)
        return (g_lang == LANG_EN) ? L"Error: cannot open file(s)\n" : L"Error: no se pueden abrir archivo(s)\n";
    if (strcmp(key, "fileid_added") == 0)
        return (g_lang == LANG_EN) ? L"file_id.diz added successfully.\n" : L"file_id.diz a\u00f1adido correctamente.\n";
    if (strcmp(key, "error_patch_has_fileid") == 0)
        return (g_lang == LANG_EN) ? L"Error: patch already contains a file_id.diz\n" : L"Error: el parche ya contiene file_id.diz\n";
    if (strcmp(key, "filter_ppf") == 0)
        return (g_lang == LANG_EN) ? L"PPF files" : L"Archivos PPF";
    if (strcmp(key, "filter_all") == 0)
        return (g_lang == LANG_EN) ? L"All files" : L"Todos los archivos";
    if (strcmp(key, "filter_diz") == 0)
        return (g_lang == LANG_EN) ? L"DIZ files" : L"Archivos DIZ";
    if (strcmp(key, "filter_files") == 0)
        return (g_lang == LANG_EN) ? L"Files" : L"Archivos";
    if (strcmp(key, "filter_images") == 0)
        return (g_lang == LANG_EN) ? L"BIN/GI/ISO files" : L"Archivos BIN/GI/ISO";
    if (strcmp(key, "usage_apply") == 0)
        return (g_lang == LANG_EN) ? L"Usage: PPFManager.exe <command> <binfile> <patchfile>\n" : L"Uso: PPFManager.exe <command> <archivo bin> <archivo parche>\n"; 
    if (strcmp(key, "error_could_not_open_files_apply") == 0)
        return (g_lang == LANG_EN) ? L"Error: could not open files.\n" : L"Error: no se pudieron abrir los archivos.\n";
    if (strcmp(key, "ppf1_applied") == 0)
        return (g_lang == LANG_EN) ? L"PPF1 patch applied.\n" : L"Parche PPF1 aplicado.\n";
    if (strcmp(key, "ppf2_applied") == 0)
        return (g_lang == LANG_EN) ? L"PPF2 patch applied.\n" : L"Parche PPF2 aplicado.\n";
    if (strcmp(key, "ppf3_applied") == 0)
        return (g_lang == LANG_EN) ? L"PPF3 patch applied.\n" : L"Parche PPF3 aplicado.\n";
    if (strcmp(key, "ppf_apply_failed") == 0)
        return (g_lang == LANG_EN) ? L"Error: failed to apply patch.\n" : L"Error: fallo al aplicar el parche.\n";
    if (strcmp(key, "unknown_patch_version") == 0)
        return (g_lang == LANG_EN) ? L"Unknown patch version.\n" : L"Versi\u00f3n de parche desconocida.\n";
    if (strcmp(key, "ppf3_undo_applied") == 0)
        return (g_lang == LANG_EN) ? L"PPF3 patch undo applied.\n" : L"Deshacer parche PPF3 aplicado.\n";
    if (strcmp(key, "undo_supported_only_ppf3") == 0)
        return (g_lang == LANG_EN) ? L"Undo function is supported by PPF3.0 only\n" : L"La funci\u00f3n deshacer solo est\u00e1 soportada por PPF3.0\n";
    if (strcmp(key, "console_help") == 0)
        return (g_lang == LANG_EN)
            ? L"\nUsage: PPFManager.exe <Commands> <Image File> <patch>\n<Commands>\n  c : create PPF3.0 patch            f : add file_id.diz\n  s : show patch information\n  a : apply PPF1/2/3 patch\n  u : undo patch (PPF3 only)\n<Switches>\n -u : include undo data (default=off)\n -x : disable patch validation (default=off)\n -i : imagetype, 0 = BIN, 1 = GI, 2 = ISO (default=bin)\n -d : \"write description\"\n -f : \"file_id.diz\" to insert the file into the patch\n\nExamples: PPF c -u -i 0 -d \"my patch\" game.bin mod.bin output.ppf\n          PPF f patch.ppf myfileid.diz\n"
            : L"\nUso: PPFManager.exe <Comandos> <Archivo Imagen> <parche>\n<Comandos>\n  c : crear parche PPF3.0            f : a\u00f1adir file_id.diz\n  s : mostrar informaci\u00f3n del parche\n  a : aplicar parche PPF1/2/3\n  u : deshacer parche (solo PPF3)\n<Opciones>\n -u : incluir datos undo (por defecto=off)\n -x : desactivar comprobaci\u00f3n de parche (por defecto=off)\n -i : tipo de imagen, 0 = BIN, 1 = GI, 2 = ISO (por defecto=bin)\n -d : \"escribir descripci\u00f3n\"\n -f : \"file_id.diz\" para insertar el archivo en el parche\n\nEjemplos: PPF c -u -i 0 -d \"mi parche\" game.bin mod.bin output.ppf\n          PPF f patch.ppf myfileid.diz\n"; 
    if (strcmp(key, "error_unknown_command") == 0)
        return (g_lang == LANG_EN) ? L"Error: unknown command\n" : L"Error: comando desconocido\n";
    return L"";
}

// Console output helpers: format wide strings (from tw()) and print appropriately to stdout
// If stdout is a real console, use WriteConsoleW to emit Unicode directly (avoids mojibake).
// Otherwise (redirect or pipe), emit UTF-8 bytes so consumers receive UTF-8.
static void ConsolePutW(const wchar_t *w) {
    if (!w) return;
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    if (hOut && hOut != INVALID_HANDLE_VALUE && GetConsoleMode(hOut, &mode)) {
        DWORD written = 0;
        WriteConsoleW(hOut, w, (DWORD)wcslen(w), &written, NULL);
        return;
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (len <= 0) return;
    char *buf = (char*)malloc(len);
    if (!buf) return;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, len, NULL, NULL);
    fputs(buf, stdout);
    free(buf);
}

static void ConsolePrintfKey(const char *key, ...) {
    const wchar_t *fmtW = tw(key);
    if (!fmtW || wcslen(fmtW) == 0) return;
    wchar_t tmp[4096];
    va_list ap;
    va_start(ap, key);
    _vsnwprintf(tmp, sizeof(tmp)/sizeof(wchar_t), fmtW, ap);
    va_end(ap);
    ConsolePutW(tmp);
}

// Convenience: format translation `key` with a UTF-8 multi-byte `arg` (common for argv/__DATE__)
static void ConsolePrintfKeyMB(const char *key, const char *mb) {
    const wchar_t *fmtW = tw(key);
    if (!fmtW || wcslen(fmtW) == 0) return;
    if (!mb) {
        ConsolePutW(fmtW);
        return;
    }
    int wlen = MultiByteToWideChar(CP_UTF8, 0, mb, -1, NULL, 0);
    if (wlen <= 0) {
        // fallback: try ANSI
        wlen = MultiByteToWideChar(CP_ACP, 0, mb, -1, NULL, 0);
    }
    if (wlen <= 0) return;
    wchar_t *argW = (wchar_t*)malloc(wlen * sizeof(wchar_t));
    if (!argW) return;
    if (!MultiByteToWideChar(CP_UTF8, 0, mb, -1, argW, wlen)) {
        // fallback to ANSI
        MultiByteToWideChar(CP_ACP, 0, mb, -1, argW, wlen);
    }
    wchar_t tmp[4096];
    _snwprintf_s(tmp, sizeof(tmp)/sizeof(wchar_t), _TRUNCATE, fmtW, argW);
    ConsolePutW(tmp);
    free(argW);
}

// Set g_lang based on system UI language (used in console mode when no INI loaded)
static void SetLangFromSystem(void) {
    LANGID lid = GetUserDefaultUILanguage();
    WORD primary = PRIMARYLANGID(lid);
    if (primary == LANG_SPANISH) g_lang = LANG_ES;
    else if (primary == LANG_ENGLISH) g_lang = LANG_EN;
}

// Ensure the console cursor ends on a fresh line without injecting fake input.
static void ConsoleEnsureNewline(void) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!hOut || hOut == INVALID_HANDLE_VALUE) return;
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(hOut, &info)) return;
    if (info.dwCursorPosition.X != 0) {
        DWORD written = 0;
        WriteConsoleW(hOut, L"\r\n", 2, &written, NULL);
    }
}

typedef struct { const wchar_t *id; const wchar_t *es; const wchar_t *en; } UI_TEXT_ENTRY;
static const UI_TEXT_ENTRY UI_TEXTS[] = {
    {L"tab_create", L" Crear Parche ", L" Create Patch "},
    {L"tab_apply", L" Aplicar Parche ", L" Apply Patch "},
    {L"menu_lang", L"Idioma", L"Language"},
    {L"menu_theme", L"Tema", L"Theme"},
    {L"menu_help", L"Ayuda", L"Help"},
    {L"menu_help_show", L"Ayuda", L"Help"},
    {L"menu_about", L"Acerca de", L"About"},
    {L"menu_es", L"Español", L"Spanish"},
    {L"menu_en", L"Inglés", L"English"},
    {L"menu_dark", L"Oscuro", L"Dark"},
    {L"menu_light", L"Claro", L"Light"},
    {L"lbl_img", L"Imagen original:", L"Original image:"},
    {L"lbl_mod", L"Imagen modificada:", L"Modified image:"},
    {L"lbl_ppf_dest", L"Archivo PPF:", L"PPF file:"},
    {L"lbl_diz", L"File_id.diz:", L"File_id.diz:"},
    {L"lbl_desc", L"Descripción:", L"Description:"},
    {L"chk_undo", L"Incluir datos de deshacer", L"Include undo data"},
    {L"chk_valid", L"Activar validación", L"Enable validation"},
    {L"lbl_tipo", L"Imagen:", L"Image:"},
    {L"btn_create", L"Crear Parche", L"Create Patch"},
    {L"btn_show", L"Info Parche", L"Patch Info"},
    {L"btn_add", L"Añadir file_id", L"Add file_id.diz"},
    {L"btn_clear", L"Limpiar", L"Clear"},
    {L"lbl_img_apply", L"Imagen original:", L"Original image:"},
    {L"lbl_ppf_apply", L"Archivo PPF:", L"PPF file:"},
    {L"chk_revert", L"Deshacer parche", L"Undo patch"},
    {L"btn_apply", L"Aplicar Parche", L"Apply Patch"},
    {L"btn_clear_apply", L"Limpiar", L"Clear"},
    {L"lbl_salida_apply", L"Salida:", L"Output:"},
    {L"help_title", L"Ayuda de PPFManager", L"PPFManager Help"},
    {L"help_interface", L"Pestaña Crear Parche:\n  - Imagen original — Seleccionar la imagen sin modificar.\n  - Imagen modificada — Seleccionar la imagen modificada.\n  - Archivo PPF — Elegir el nombre del parche, se agrega automáticamente al añadir la Imagen original.\n  - File_id.diz (opcional) — Archivo que contiene texto informativo del parche y se inserta dentro del .ppf\n  - Descripción (opcional) — Añade una descripción y se inserta dentro del .ppf\n Opciones:\n  - Incluir datos deshacer — Incluye información para revertir los cambios realizados y devolver la imagen original.\n  - Activar validación — Protección para asegurar que no se pueda aplicar el parche en una imagen diferente\n  - Imagen — Elegir tipo de imagen, .bin .gi .iso\n\n  - Botón Crear Parche — Ejecuta la creación del parche en la ubicacion establecida.\n  - Botón Info Parche — Muestra la información de archivo .ppf agregado en el campo 'Archivo PPF'.\n  - Botón Añadir file_id — Añade un archivo file_id.diz al parche.\n  - Botón Limpiar — Limpia la salida de la consola.\n\nPestaña Aplicar Parche:\n  - Imagen original — Seleccionar la imagen original\n  - Archivo PPF — Seleccionar el parche para aplicar en la imagen\n  - Deshacer parche — Revierte la aplicación del parche. (el .ppf tiene que haber sido creado con estos datos)", L"Create Patch tab:\n  - Original image — Select the unmodified image.\n  - Modified image — Select the modified image.\n  - PPF file — Choose the patch name, it is added automatically when selecting the Original image.\n  - File_id.diz (optional) — File that contains informative text about the patch and is inserted inside the .ppf\n  - Description (optional) — Adds a description and is inserted inside the .ppf\n Options:\n  - Include undo data — Includes information to revert the changes made and restore the original image.\n  - Enable validation — Protection to ensure that the patch cannot be applied to a different image\n  - Image — Choose image type, .bin .gi .iso\n\n  - Create Patch button — Executes the patch creation at the established location.\n  - Patch Info button — Displays the .ppf file information added in the 'PPF file' field.\n  - Add file_id button — Adds a file_id.diz file to the patch.\n  - Clear button — Clears the console output.\n\nApply Patch tab:\n  - Original image — Select the original image\n  - PPF file — Select the patch to apply to the image\n  - Undo patch — Reverts the application of the patch. (the .ppf must have been created with these data)"},
};
static const wchar_t* T(const wchar_t* id) {
    for (size_t i = 0; i < sizeof(UI_TEXTS)/sizeof(UI_TEXTS[0]); ++i) {
        if (wcscmp(UI_TEXTS[i].id, id) == 0)
            return (g_lang == LANG_EN) ? UI_TEXTS[i].en : UI_TEXTS[i].es;
    }
    return id;
}

// Traduce todos los controles de la interfaz según g_lang
static void TranslateUI(HWND hwndTab, HWND hCrearPanel, HWND hAplicarPanel,
    HWND hCrearLblImg, HWND hCrearLblMod, HWND hCrearLblPPF, HWND hCrearLblDIZ, HWND hCrearLblDesc,
    HWND hCrearChkUndo, HWND hCrearChkValid, HWND hCrearLblTipo, HWND hCrearComboTipo, HWND hCrearBtnCrear, HWND hCrearBtnShow, HWND hCrearBtnAdd, HWND hCrearBtnClear, HWND hCrearLblSalida,
    HWND hAplicarLblImg, HWND hAplicarLblPPF, HWND hAplicarChkRevert, HWND hAplicarBtnApply, HWND hAplicarBtnClear, HWND hAplicarLblSalida,
    HMENU hMenuBar, HMENU hMenuIdioma, HMENU hMenuTema, HMENU hMenuAyuda) {
    // Tabs
    (void)hCrearLblSalida;
    TCITEMW tie = {0};
    tie.mask = TCIF_TEXT;
    tie.pszText = (LPWSTR)T(L"tab_create");
    TabCtrl_SetItem(hwndTab, 0, &tie);
    tie.pszText = (LPWSTR)T(L"tab_apply");
    TabCtrl_SetItem(hwndTab, 1, &tie);
    // Crear panel
    SetWindowTextW(hCrearLblImg, T(L"lbl_img"));
    SetWindowTextW(hCrearLblMod, T(L"lbl_mod"));
    SetWindowTextW(hCrearLblPPF, T(L"lbl_ppf_dest"));
    SetWindowTextW(hCrearLblDIZ, T(L"lbl_diz"));
    SetWindowTextW(hCrearLblDesc, T(L"lbl_desc"));
    SetWindowTextW(hCrearChkUndo, T(L"chk_undo"));
    SetWindowTextW(hCrearChkValid, T(L"chk_valid"));
    SetWindowTextW(hCrearLblTipo, T(L"lbl_tipo"));
    SetWindowTextW(hCrearBtnCrear, T(L"btn_create"));
    SetWindowTextW(hCrearBtnShow, T(L"btn_show"));
    SetWindowTextW(hCrearBtnAdd, T(L"btn_add"));
    SetWindowTextW(hCrearBtnClear, T(L"btn_clear"));
    // "Salida" label removed; no text to set here
    // Combo tipo
    SendMessageW(hCrearComboTipo, CB_RESETCONTENT, 0, 0);
    SendMessageW(hCrearComboTipo, CB_ADDSTRING, 0, (LPARAM)L"BIN");
    SendMessageW(hCrearComboTipo, CB_ADDSTRING, 0, (LPARAM)L"GI");
    SendMessageW(hCrearComboTipo, CB_ADDSTRING, 0, (LPARAM)L"ISO");
    SendMessageW(hCrearComboTipo, CB_SETCURSEL, 0, 0); // Seleccionar BIN siempre
    // Aplicar panel
    SetWindowTextW(hAplicarLblImg, T(L"lbl_img_apply"));
    SetWindowTextW(hAplicarLblPPF, T(L"lbl_ppf_apply"));
    SetWindowTextW(hAplicarChkRevert, T(L"chk_revert"));
    SetWindowTextW(hAplicarBtnApply, T(L"btn_apply"));
    SetWindowTextW(hAplicarBtnClear, T(L"btn_clear_apply"));
    SetWindowTextW(hAplicarLblSalida, T(L"lbl_salida_apply"));
    // Menús
    ModifyMenuW(hMenuBar, 0, MF_BYPOSITION | MF_STRING, (UINT_PTR)hMenuIdioma, T(L"menu_lang"));
    ModifyMenuW(hMenuBar, 1, MF_BYPOSITION | MF_STRING, (UINT_PTR)hMenuTema, T(L"menu_theme"));
    ModifyMenuW(hMenuBar, 2, MF_BYPOSITION | MF_STRING, (UINT_PTR)hMenuAyuda, T(L"menu_help"));
    ModifyMenuW(hMenuIdioma, 0, MF_BYPOSITION | MF_STRING, 301, T(L"menu_es"));
    ModifyMenuW(hMenuIdioma, 1, MF_BYPOSITION | MF_STRING, 302, T(L"menu_en"));
    ModifyMenuW(hMenuTema, 0, MF_BYPOSITION | MF_STRING, 203, T(L"menu_dark"));
    ModifyMenuW(hMenuTema, 1, MF_BYPOSITION | MF_STRING, 204, T(L"menu_light"));
    ModifyMenuW(hMenuAyuda, 1, MF_BYPOSITION | MF_STRING, 206, T(L"menu_about"));
    ModifyMenuW(hMenuAyuda, 0, MF_BYPOSITION | MF_STRING, 205, T(L"menu_help_show"));
    // Título ventana
    SetWindowTextW(g_hwndMain, L"PPF Manager");
}

// Enum child callback: translate static/control text if matches known Spanish/English text

static void ForceLayoutRefresh(void) {
    if (!g_hwndMain) return;
    RECT rc;
    GetClientRect(g_hwndMain, &rc);
    LPARAM lparam = (LPARAM)((rc.right & 0xFFFF) | ((rc.bottom & 0xFFFF) << 16));
    SendMessageW(g_hwndMain, WM_SIZE, 0, lparam);
    if (IsWindow(g_hCrearPanel)) {
        InvalidateRect(g_hCrearPanel, NULL, TRUE);
        UpdateWindow(g_hCrearPanel);
    }
    if (IsWindow(g_hAplicarPanel)) {
        InvalidateRect(g_hAplicarPanel, NULL, TRUE);
        UpdateWindow(g_hAplicarPanel);
    }
}

// Safe append with quote to wide buffer, ensures spacing between args
static void AppendQuotedArg(wchar_t *dst, size_t dstSize, const wchar_t *arg) {
    if (!arg || !dst) return;
    if (wcslen(arg) == 0) return;
    size_t cur = wcslen(dst);
    if (cur + 3 >= dstSize) return;
    // add space if needed
    if (cur > 0 && dst[cur-1] != L' ') {
        wcscat(dst, L" ");
        cur++;
    }
    // append quoted arg
    wcscat(dst, L"\"");
    // ensure not overflow
    size_t avail = dstSize - cur - 3; // for quotes and null
    wcsncat(dst, arg, avail);
    wcscat(dst, L"\"");
}

// Build command line for Create operation from controls
static void BuildCreateCmdLine(wchar_t *out, size_t outSize, HWND hImg, HWND hMod, HWND hPPF, HWND hDIZ, HWND hDesc, HWND hChkUndo, HWND hChkValid, HWND hComboTipo) {
    if (!out || outSize < 20) return;
    out[0] = 0;
    wcscpy_s(out, outSize, L"MakePPF");
    // command 'c' = create (exe, command, flags, files)
    wcscat_s(out, outSize, L" c");
    wchar_t buf[MAX_PATH+4];
    // Undo == -u
    if (hChkUndo && (SendMessageW(hChkUndo, BM_GETCHECK, 0, 0) == BST_CHECKED)) wcscat_s(out, outSize, L" -u");
    // Validation checkbox means 'activate validation' -> no flag; if unchecked, pass -x to disable
    if (hChkValid && (SendMessageW(hChkValid, BM_GETCHECK, 0, 0) != BST_CHECKED)) wcscat_s(out, outSize, L" -x");
    // type: BIN=0, GI=1, ISO=2 -> use -i <0|1|2>
    if (hComboTipo) {
        int sel = (int)SendMessageW(hComboTipo, CB_GETCURSEL, 0, 0);
        int itype = (sel >= 0 && sel <= 2) ? sel : 0; // default to BIN if out of range
        wchar_t itbuf[8];
        _snwprintf_s(itbuf, sizeof(itbuf)/sizeof(wchar_t), _TRUNCATE, L"%d", itype);
        wcscat_s(out, outSize, L" -i ");
        AppendQuotedArg(out, outSize, itbuf);
    }
    if (hDesc && GetWindowTextW(hDesc, buf, MAX_PATH) && wcslen(buf) > 0) {
        if (wcslen(buf) > 50) buf[50] = 0; /* Ensure description <= 50 chars */
        wcscat_s(out, outSize, L" -d ");
        AppendQuotedArg(out, outSize, buf);
    }
    if (hDIZ && GetWindowTextW(hDIZ, buf, MAX_PATH) && wcslen(buf) > 0) {
        wcscat_s(out, outSize, L" -f ");
        AppendQuotedArg(out, outSize, buf);
    }
    // finally append the positional file arguments (img, mod, ppf) as in Java GUI
    if (hImg && GetWindowTextW(hImg, buf, MAX_PATH)) AppendQuotedArg(out, outSize, buf);
    if (hMod && GetWindowTextW(hMod, buf, MAX_PATH)) AppendQuotedArg(out, outSize, buf);
    if (hPPF && GetWindowTextW(hPPF, buf, MAX_PATH)) AppendQuotedArg(out, outSize, buf);
}

// Build command line for Apply operation from controls
static void BuildApplyCmdLine(wchar_t *out, size_t outSize, HWND hImg, HWND hPPF, HWND hChkRevert) {
    if (!out) return;
    out[0] = 0;
    wcscpy(out, L"ApplyPPF");
    // Use 'a' apply or 'u' undo as command (ApplyPPF expects command first)
    if (hChkRevert && (SendMessageW(hChkRevert, BM_GETCHECK, 0, 0) == BST_CHECKED)) {
        wcscat(out, L" u");
    } else {
        wcscat(out, L" a");
    }
    wchar_t buf[MAX_PATH+4];
    if (hImg && GetWindowTextW(hImg, buf, MAX_PATH)) AppendQuotedArg(out, outSize, buf);
    if (hPPF && GetWindowTextW(hPPF, buf, MAX_PATH)) AppendQuotedArg(out, outSize, buf);
}

// Extract parent folder (directory) from a path into out (including no trailing slash)
static void GetParentFolder(const wchar_t *path, wchar_t *out, size_t outSize) {
    if (!path || !out) return;
    const wchar_t *p = wcsrchr(path, L'\\');
    if (!p) p = wcsrchr(path, L'/');
    if (!p) {
        // no folder, use current dir
        GetCurrentDirectoryW((DWORD)outSize, out);
        return;
    }
    size_t len = p - path;
    if (len >= outSize) len = outSize - 1;
    wcsncpy(out, path, len);
    out[len] = 0;
}

// Get the directory where PPFManager.exe is located
static void GetExecutableDirectory(wchar_t *out, size_t outSize) {
    if (!out || outSize == 0) return;
    if (GetModuleFileNameW(NULL, out, (DWORD)outSize) > 0) {
        wchar_t *lastSlash = wcsrchr(out, L'\\');
        if (lastSlash) {
            *lastSlash = 0; // Remove filename, keep directory
        }
    } else {
        // Fallback to current directory
        GetCurrentDirectoryW((DWORD)outSize, out);
    }
}

// Set image type to BIN/GI/ISO (combo) and adjust validation checkbox when a path points to a known image extension
static void MaybeSetImageTypeFromPath(HWND hComboTipo, HWND hChkValid, const wchar_t *path) {
    if (!path || wcslen(path) == 0) return;
    const wchar_t *dot = wcsrchr(path, L'.');
    if (!dot) return;
    /* Recognized extensions (case-insensitive): .bin -> BIN (0), .gi -> GI (1), .iso -> ISO (2) */
    if (_wcsicmp(dot, L".iso") == 0) {
        if (hComboTipo) SendMessageW(hComboTipo, CB_SETCURSEL, 2, 0);
        if (!g_crearValidUserSet && hChkValid) SendMessageW(hChkValid, BM_SETCHECK, BST_UNCHECKED, 0);
    } else if (_wcsicmp(dot, L".gi") == 0) {
        if (hComboTipo) SendMessageW(hComboTipo, CB_SETCURSEL, 1, 0);
        if (!g_crearValidUserSet && hChkValid) SendMessageW(hChkValid, BM_SETCHECK, BST_CHECKED, 0);
    } else if (_wcsicmp(dot, L".bin") == 0) {
        if (hComboTipo) SendMessageW(hComboTipo, CB_SETCURSEL, 0, 0);
        if (!g_crearValidUserSet && hChkValid) SendMessageW(hChkValid, BM_SETCHECK, BST_CHECKED, 0);
    }
}

// Get path to settings INI in %APPDATA%\MakePPF\settings.ini
static void GetSettingsFilePath(wchar_t *out, size_t outSize) {
    wchar_t appdata[MAX_PATH] = {0};
    DWORD len = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        // fallback to current dir
        GetCurrentDirectoryW(MAX_PATH, appdata);
    }
    // Ensure folder exists
    wchar_t folder[MAX_PATH];
    _snwprintf_s(folder, MAX_PATH, _TRUNCATE, L"%s\\MakePPF", appdata);
    CreateDirectoryW(folder, NULL);
    _snwprintf_s(out, outSize, _TRUNCATE, L"%s\\settings.ini", folder);
}

// Load settings from INI and populate controls (tolerant)
static void LoadSettings(HWND hCrearEditImg, HWND hCrearEditMod, HWND hCrearEditPPF, HWND hCrearEditDIZ, HWND hCrearEditDesc, HWND hCrearChkUndo, HWND hCrearChkValid, HWND hCrearComboTipo,
                         HWND hAplicarEditImg, HWND hAplicarEditPPF, HWND hAplicarChkRevert) {
    wchar_t inipath[MAX_PATH];
    /* Parametros intencionalmente no usados (se mantienen por compatibilidad de firma) */
    (void)hCrearEditImg; (void)hCrearEditMod; (void)hCrearEditPPF; (void)hCrearEditDIZ; (void)hAplicarEditImg; (void)hAplicarEditPPF; 
    GetSettingsFilePath(inipath, MAX_PATH);
    // Crear: do NOT restore saved paths or description. Checks are NOT persisted.
    // Default behavior: all checkboxes disabled except 'Activar validación' which defaults to checked.
    if (hCrearEditDesc) SetWindowTextW(hCrearEditDesc, L"");
    if (hCrearChkUndo) SendMessageW(hCrearChkUndo, BM_SETCHECK, BST_UNCHECKED, 0);
    if (hCrearChkValid) SendMessageW(hCrearChkValid, BM_SETCHECK, BST_CHECKED, 0);
    if (hCrearComboTipo) SendMessageW(hCrearComboTipo, CB_SETCURSEL, 0, 0);
    // Aplicar: do NOT restore saved paths. Ensure revert checkbox is unchecked by default.
    if (hAplicarChkRevert) SendMessageW(hAplicarChkRevert, BM_SETCHECK, BST_UNCHECKED, 0);
    // Restaurar solo la posición de la ventana si está presente (no el tamaño)
    {
        int hasPos = GetPrivateProfileIntW(L"Window", L"HasPos", 0, inipath);
        if (hasPos && g_hwndMain) {
            int left = GetPrivateProfileIntW(L"Window", L"Left", CW_USEDEFAULT, inipath);
            int top = GetPrivateProfileIntW(L"Window", L"Top", CW_USEDEFAULT, inipath);
            // Ignore saved positions that indicate a minimized window (Windows uses -32000) or clearly invalid values
            if (left == CW_USEDEFAULT || top == CW_USEDEFAULT || left <= -32000 || top <= -32000) {
                /* ignore minimized/invalid saved pos */
            } else {
                // Ensure position within reasonable screen bounds before applying
                int screenW = GetSystemMetrics(SM_CXSCREEN);
                int screenH = GetSystemMetrics(SM_CYSCREEN);
                if (!(left < -screenW || left > screenW * 2 || top < -screenH || top > screenH * 2)) {
                    // Restaurar SOLO posición (no forzar tamaño; evita recortes en DPI alto)
                    SetWindowPos(g_hwndMain, NULL, left, top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                }
            }
        }
    }
    // Restore language selection (0 = es, 1 = en)
    {
        int langEn = GetPrivateProfileIntW(L"Window", L"LangEn", 0, inipath);
        g_lang = langEn ? LANG_EN : LANG_ES;
        // Refrescar menú de idioma (check en el idioma activo)
        if (g_hwndMain) {
            HMENU hMenuBar = GetMenu(g_hwndMain);
            if (hMenuBar) {
                HMENU hMenuIdioma = GetSubMenu(hMenuBar, 0);
                if (hMenuIdioma) {
                    CheckMenuItem(hMenuIdioma, 201, MF_BYCOMMAND | (g_lang == LANG_ES ? MF_CHECKED : MF_UNCHECKED));
                    CheckMenuItem(hMenuIdioma, 202, MF_BYCOMMAND | (g_lang == LANG_EN ? MF_CHECKED : MF_UNCHECKED));
                }
            }
        }
    }
    // Restore theme preference (0 claro, 1 oscuro)
    g_themePref = GetPrivateProfileIntW(L"Window", L"ThemeDark", 0, inipath) ? 1 : 0;
}

// Save settings from controls to INI
static void SaveSettings(HWND hCrearEditImg, HWND hCrearEditMod, HWND hCrearEditPPF, HWND hCrearEditDIZ, HWND hCrearEditDesc, HWND hCrearChkUndo, HWND hCrearChkValid, HWND hCrearComboTipo,
                         HWND hAplicarEditImg, HWND hAplicarEditPPF, HWND hAplicarChkRevert) {
    wchar_t inipath[MAX_PATH];
    /* Ninguno de los parámetros de control se persiste; marcaremos explícitamente como no usados */
    (void)hCrearEditImg; (void)hCrearEditMod; (void)hCrearEditPPF; (void)hCrearEditDIZ; (void)hCrearEditDesc; (void)hCrearChkUndo; (void)hCrearChkValid; (void)hCrearComboTipo; (void)hAplicarEditImg; (void)hAplicarEditPPF; (void)hAplicarChkRevert;
    GetSettingsFilePath(inipath, MAX_PATH);
    // Do NOT save path fields or control/check states. Only save window position on exit.
    if (g_hwndMain) {
        // Prefer the normal (restored) position so we don't persist minimized coordinates
        WINDOWPLACEMENT wp = {0}; wp.length = sizeof(wp);
        if (GetWindowPlacement(g_hwndMain, &wp)) {
            RECT rc = wp.rcNormalPosition;
            int left = rc.left; int top = rc.top;
            wchar_t sbuf[64];
            _snwprintf_s(sbuf, 64, _TRUNCATE, L"%d", left); WritePrivateProfileStringW(L"Window", L"Left", sbuf, inipath);
            _snwprintf_s(sbuf, 64, _TRUNCATE, L"%d", top); WritePrivateProfileStringW(L"Window", L"Top", sbuf, inipath);
            WritePrivateProfileStringW(L"Window", L"HasPos", L"1", inipath);
        } else {
            // fallback: use current window rect if placement failed
            RECT rc; GetWindowRect(g_hwndMain, &rc);
            int left = rc.left; int top = rc.top;
            wchar_t sbuf[64];
            _snwprintf_s(sbuf, 64, _TRUNCATE, L"%d", left); WritePrivateProfileStringW(L"Window", L"Left", sbuf, inipath);
            _snwprintf_s(sbuf, 64, _TRUNCATE, L"%d", top); WritePrivateProfileStringW(L"Window", L"Top", sbuf, inipath);
            WritePrivateProfileStringW(L"Window", L"HasPos", L"1", inipath);
        }
    }
    // Save language selection
    {
        wchar_t lbuf[4];
        _snwprintf_s(lbuf, 4, _TRUNCATE, L"%d", (g_lang == LANG_EN) ? 1 : 0);
        WritePrivateProfileStringW(L"Window", L"LangEn", lbuf, inipath);
    }
    // Save theme preference (0 claro, 1 oscuro)
    {
        wchar_t tbuf[4];
        _snwprintf_s(tbuf, 4, _TRUNCATE, L"%d", g_themePref ? 1 : 0);
        WritePrivateProfileStringW(L"Window", L"ThemeDark", tbuf, inipath);
    }
}

// Append Unicode text to an edit control (must be called from UI thread)
static void AppendTextToEdit(HWND hEdit, const wchar_t *text) {
    if (!IsWindow(hEdit) || !text) return;
    // set selection to end
    int len = GetWindowTextLengthW(hEdit);
    SendMessageW(hEdit, EM_SETSEL, len, len);
    SendMessageW(hEdit, EM_REPLACESEL, FALSE, (LPARAM)text);
    // scroll to caret
    SendMessageW(hEdit, EM_SCROLLCARET, 0, 0);
}

// Replace all occurrences of a wide substring with another, returning a newly allocated string
static wchar_t* ReplaceAllWide(const wchar_t *src, const wchar_t *find, const wchar_t *repl) {
    if (!src || !find || !repl) return NULL;
    const wchar_t *p = src;
    int count = 0;
    size_t findLen = wcslen(find);
    size_t replLen = wcslen(repl);
    while ((p = wcsstr(p, find))) { count++; p += findLen; }
    size_t srcLen = wcslen(src);
    size_t newLen = srcLen + (replLen > findLen ? (replLen - findLen) * count : 0) + 1;
    wchar_t *out = (wchar_t*)malloc((newLen) * sizeof(wchar_t));
    if (!out) return NULL;
    wchar_t *dst = out;
    const wchar_t *cur = src;
    while ((p = wcsstr(cur, find))) {
        size_t n = p - cur;
        // copy chunk before match
        wmemcpy(dst, cur, n);
        dst += n;
        // copy replacement
        wmemcpy(dst, repl, replLen);
        dst += replLen;
        cur = p + findLen;
    }
    // copy tail
    if (*cur) {
        size_t tail = wcslen(cur);
        wmemcpy(dst, cur, tail);
        dst += tail;
    }
    *dst = 0;
    return out;
}

// Single-pass multi-replacer (checks case-sensitive first then case-insensitive in rule order)
// This avoids multiple full-text passes when applying many small replacements.
// Centralized, deduplicated translation tables are defined below so all UI strings live in one place
// for easier maintenance and future i18n extraction.
typedef struct { const wchar_t *find; const wchar_t *repl; } TranslationRule;

static wchar_t *ReplaceMultipleWideSinglePass(const wchar_t *src, const TranslationRule *rules, size_t n) {
    if (!src || !rules || n == 0) return NULL;
    size_t srcLen = wcslen(src);
    // start with a reasonably sized buffer
    size_t outCap = srcLen + 64;
    wchar_t *out = (wchar_t*)malloc((outCap + 1) * sizeof(wchar_t));
    if (!out) return NULL;
    size_t outLen = 0;

    size_t pos = 0;
    while (pos < srcLen) {
        int matched = 0;
        // First try case-sensitive rules in order
        for (size_t i = 0; i < n; ++i) {
            const wchar_t *find = rules[i].find;
            const wchar_t *repl = rules[i].repl;
            if (!find || !*find) continue;
            size_t fl = wcslen(find);
            if (pos + fl <= srcLen && wcsncmp(src + pos, find, fl) == 0) {
                size_t rl = wcslen(repl);
                // ensure capacity
                if (outLen + rl + 1 > outCap) {
                    while (outLen + rl + 1 > outCap) outCap *= 2;
                    wchar_t *nb = (wchar_t*)realloc(out, (outCap + 1) * sizeof(wchar_t));
                    if (!nb) { free(out); return NULL; }
                    out = nb;
                }
                memcpy(out + outLen, repl, rl * sizeof(wchar_t));
                outLen += rl;
                pos += fl;
                matched = 1;
                break; // apply single match and continue at new position
            }
        }
        if (matched) continue;

        // Then try case-insensitive rules in order (skip no-op case-insensitive equalities)
        for (size_t i = 0; i < n; ++i) {
            const wchar_t *find = rules[i].find;
            const wchar_t *repl = rules[i].repl;
            if (!find || !*find) continue;
            if (_wcsicmp(find, repl) == 0) continue; // preserve previous behavior (no-op CI replacement)
            size_t fl = wcslen(find);
            if (pos + fl <= srcLen) {
                int ok = 1;
                for (size_t k = 0; k < fl; ++k) {
                    if (towlower((wint_t)src[pos + k]) != towlower((wint_t)find[k])) { ok = 0; break; }
                }
                if (ok) {
                    size_t rl = wcslen(repl);
                    if (outLen + rl + 1 > outCap) {
                        while (outLen + rl + 1 > outCap) outCap *= 2;
                        wchar_t *nb = (wchar_t*)realloc(out, (outCap + 1) * sizeof(wchar_t));
                        if (!nb) { free(out); return NULL; }
                        out = nb;
                    }
                    memcpy(out + outLen, repl, rl * sizeof(wchar_t));
                    outLen += rl;
                    pos += fl;
                    matched = 1;
                    break;
                }
            }
        }
        if (matched) continue;

        // No rule matched at this position; copy single character
        if (outLen + 2 > outCap) {
            outCap *= 2;
            wchar_t *nb = (wchar_t*)realloc(out, (outCap + 1) * sizeof(wchar_t));
            if (!nb) { free(out); return NULL; }
            out = nb;
        }
        out[outLen++] = src[pos++];
    }
    out[outLen] = 0;
    return out;
}

// Centralized, deduplicated translation rules (English -> Spanish)
static const TranslationRule g_translation_rules[] = {
    { L"Writing header...", L"Escribiendo cabecera..." },
    { L"Adding file_id.diz...", L"Añadiendo file_id.diz..." },
    { L"Finding differences...", L"Buscando diferencias..." },
    { L"Progress:", L"Progreso:" },
    { L"entries found", L"entradas encontradas" },
    { L"entries", L"entradas" },
    { L"Error: insufficient memory available", L"Error: memoria insuficiente disponible" },
    { L"Error: filesize of bin file is zero!", L"Error: el tamaño del archivo bin es cero!" },
    { L"Error: input files are different in size.", L"Error: los archivos de entrada tienen distinto tamaño." },
    { L"Error: short read of bin validation block", L"Error: lectura corta del bloque de validación bin" },
    { L"Warning: short read of bin validation block", L"Aviso: lectura corta del bloque de validación bin" },
    { L"Error: need more input for command", L"Error: falta entrada para el comando" },
    { L"Error: cannot open file \"", L"Error: no se puede abrir el archivo \"" },
    { L"Error: cannot open file ", L"Error: no se puede abrir el archivo " },
    { L"Error: file ", L"Error: el archivo " },
    { L"Error: patch already contains a file_id.diz", L"Error: el parche ya contiene file_id.diz" },
    { L"Error: cannot create temp file for", L"Error: no se puede crear archivo temporal para" },
    { L"Showing patchinfo", L"Mostrando información del parche" },
    { L"Enabled", L"Habilitado." },
    { L"Disabled", L"Deshabilitado." },
    { L"Done.", L"Completado." },
    { L"Patching...", L"Parcheando..." },
    { L"Patching ...", L"Parcheando ..." },
    { L"Patch Information:", L"Información del parche:" },
    { L"Patchfile is a PPF3.0 patch.", L"El archivo es un parche PPF3.0." },
    { L"Patchfile is a PPF1.0 patch. Patch Information:", L"El archivo es un parche PPF1.0. Información del parche:" },
    { L"Patchfile is a PPF2.0 patch. Patch Information:", L"El archivo es un parche PPF2.0. Información del parche:" },
    { L"The size of the bin file isn't correct, continue ? (y/n): ", L"El tamaño del archivo bin no es correcto, \u00BFcontinuar? (s/n): " },
    { L"Binblock/Patchvalidation failed. ISO images sometimes require validation disabled (-x). continue ? (y/n): ", L"La validaci\u00f3n del bloque bin fall\u00f3. Las im\u00e1genes ISO a veces requieren desactivar la validaci\u00f3n (-x). \u00BFContinuar? (s/n): " },
    { L"Binblock/Patchvalidation failed. continue ? (y/n): ", L"La validaci\u00f3n del bloque bin fall\u00f3. \u00BFContinuar? (s/n): " },
    { L"Not available", L"No disponible" },
    { L"Available", L"Disponible" },
    { L"unknown command", L"Comando desconocido" },
    { L"Executing:", L"Ejecutando:" },
    { L"Aborted...", L"Abortado..." },
    { L"Usage: PPF <command> [-<sw> [-<sw>...]] <original bin> <modified bin> <ppf>", L"Uso: PPF <comando> [-<sw> [-<sw>...]] <Imagen original> <Imagen modificado> <ppf>" },
    { L"<Commands>", L"<Comandos>" },
    { L"  c : create PPF3.0 patch            f : add file_id.diz", L"  c : crear parche PPF3.0            f : añadir file_id.diz" },
    { L"  s : show patchinfomation", L"  s : mostrar información del parche" },
    { L"<Switches>", L"<Interruptores>" },
    { L" -u : include undo data (default=off)", L" -u        : incluir datos de deshacer (por defecto=apagado)" },
    { L" -x : disable patchvalidation (default=off)", L" -x : deshabilitar validación de parche (por defecto=apagado)" },
    { L" -i : imagetype, 0 = BIN, 1 = GI, 2 = ISO (default=bin)", L" -i : tipo de imagen, 0 = BIN, 1 = GI, 2 = ISO (por defecto=bin)" },
    { L" -d : use \"text\" as description", L" -d : usar \"texto\" como descripción" },
    { L" -f \"file\" : add \"file\" as file_id.diz", L" -f \"archivo\" : añadir \"archivo\" como file_id.diz" },
    { L"Examples: PPF c -u -i 1 -d \"my elite patch\" game.bin patch.bin output.ppf", L"Ejemplos: PPF c -u -i 1 -d \"mi parche elite\" juego.bin parche.bin salida.ppf" },
    { L"          PPF f patch.ppf myfileid.diz", L"          PPF f patch.ppf fileid.diz" },
    { L"Usage: PPFManager.exe <command> <binfile> <patchfile>", L"Uso: PPFManager.exe <comando> <archivo bin> <archivo parche>" },
    { L"  a : apply PPF1/2/3 patch", L"  a : aplicar parche PPF1/2/3" },
    { L"  u : undo patch (PPF3 only)", L"  u : deshacer parche (solo PPF3)" }
};

/* Centralized, deduplicated label pairs used for label-preserving replacements and alignment.
   Keep idempotent: applying replacements multiple times has no further effect (same mapping).
*/
static const struct { const wchar_t *en; const wchar_t *es; } g_label_pairs[] = {
    { L"Version", L"Versión" },
    { L"Enc.Method", L"Método Enc." },
    { L"Imagetype", L"Tipo de imagen" },
    { L"Validation", L"Validación" },
    { L"Undo Data", L"Datos Deshacer" },
    { L"Description", L"Descripción" },
    { L"File_id.diz", L"File_id.diz" }
};

/* Notes:
 - Rules ordering matters for idempotence; rules are applied left-to-right and are designed so
   that reapplying translations is a no-op (idempotent mapping).
 - Keep these tables in sync with tests and make sure to add any new UI-facing string here
   so translation tooling can extract them easily.
*/
// Extract just filename from a path in a command line, keeping quotes
static wchar_t* StripPathsFromCommand(const wchar_t *cmdline) {
    if (!cmdline) return NULL;
    
    size_t len = wcslen(cmdline);
    wchar_t *result = (wchar_t*)malloc((len + 1) * sizeof(wchar_t));
    if (!result) return _wcsdup(cmdline);
    
    wchar_t *dst = result;
    const wchar_t *src = cmdline;
    int in_quotes = 0;
    
    while (*src) {
        if (*src == L'"') {
            in_quotes = !in_quotes;
            *dst++ = *src++;
            if (in_quotes) {
                // Inside quotes - check if this looks like a path
                const wchar_t *scan = src;
                const wchar_t *last_sep = NULL;
                int has_drive = 0;
                
                // Check for drive letter (C:, D:, etc.)
                if (scan[0] && iswalpha(scan[0]) && scan[1] == L':') {
                    has_drive = 1;
                    scan += 2; // Skip drive letter
                }
                
                // Find last separator before closing quote
                while (*scan && *scan != L'"') {
                    if (*scan == L'\\' || *scan == L'/') {
                        last_sep = scan;
                    }
                    scan++;
                }
                
                // If we found path separators, extract just the filename
                if (last_sep || has_drive) {
                    // Skip to after last separator (or after drive letter if no separators)
                    if (last_sep) {
                        src = last_sep + 1;
                    } else if (has_drive) {
                        src += 2; // Skip C:
                    }
                }
                
                // Copy rest until quote
                while (*src && *src != L'"') {
                    *dst++ = *src++;
                }
            }
        } else {
            *dst++ = *src++;
        }
    }
    *dst = 0;
    return result;
}

// Case-insensitive search for a wide needle in haystack. Returns pointer to match or NULL.
static wchar_t *wcsistr(const wchar_t *hay, const wchar_t *needle) {
    if (!hay || !needle) return NULL;
    size_t nlen = wcslen(needle);
    if (nlen == 0) return (wchar_t*)hay;
    for (const wchar_t *p = hay; *p; ++p) {
        size_t i = 0;
        while (p[i] && needle[i] && towlower(p[i]) == towlower(needle[i])) i++;
        if (i == nlen) return (wchar_t*)p;
    }
    return NULL;
}

// NOTE: Replaced by ReplaceMultipleWideSinglePass to optimize multiple replacements in one pass.
// The old case-insensitive in-place replacer is intentionally removed.

// Replace label-only occurrences like "Version   : value" preserving value. Case-insensitive label match.
static wchar_t *ReplaceLabelCI(wchar_t *buf, size_t *bufSize, const wchar_t *label_en, const wchar_t *label_es) {
    wchar_t *p = buf;
    size_t enLen = wcslen(label_en);
    size_t esLen = wcslen(label_es);
    while (1) {
        wchar_t *found = wcsistr(p, label_en);
        if (!found) break;
        // check for ':' after label (allow spaces)
        wchar_t *s = found + enLen;
        while (*s == L' ' || *s == L'\t') s++;
        if (*s == L':') {
            size_t curLen = wcslen(buf);
            size_t newLen = curLen - enLen + esLen;
            if (newLen + 1 > *bufSize) {
                size_t newSize = (*bufSize) * 2;
                if (newSize < newLen + 1) newSize = newLen + 1;
                ptrdiff_t off = found - buf;
                wchar_t *nb = (wchar_t*)realloc(buf, newSize * sizeof(wchar_t));
                if (!nb) break;
                buf = nb; *bufSize = newSize; found = buf + off;
            }
            wchar_t *after = found + enLen;
            memmove(found + esLen, after, (wcslen(after) + 1) * sizeof(wchar_t));
            memcpy(found, label_es, esLen * sizeof(wchar_t));
            p = found + esLen;
        } else {
            p = found + 1;
        }
    }
    return buf;
}

// Translate a single console line to Spanish if appropriate. Returns heap-allocated wide string (caller must free).
static wchar_t* TranslateConsoleLine(const wchar_t *line) {
    if (!line) return NULL;
    if (g_lang != LANG_ES) return _wcsdup(line);

    // Start with a copy we can modify
    size_t bufSize = wcslen(line) + 256;
    wchar_t *buf = (wchar_t*)malloc(bufSize * sizeof(wchar_t));
    if (!buf) return _wcsdup(line);
    wcscpy(buf, line);

    /* Testing hook: set PPFMANAGER_NO_SUPPRESS=1 to disable automatic suppression of help/usage blocks
       This helps debugging when output seems missing due to filtering */
    char *no_suppress = getenv("PPFMANAGER_NO_SUPPRESS");
    if (!no_suppress) {
        // Suppress console-mode help blocks in GUI: hide usage/commands/examples lines (EN/ES)
        // IMPORTANTE: Hacer esto ANTES de las traducciones para que los patrones coincidan
        const wchar_t *p = buf;
        while (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n') p++;
        const wchar_t *suppress_patterns[] = {
            L"Usage:", L"Uso:",
            L"<Commands>", L"<Comandos>",
            L"<Switches>", L"<Interruptores>", L"<Opciones>",
            L"Examples:", L"Ejemplos:", L"Example:", L"Ejemplo:",
            L"  c :", L"  s :", L"  a :", L"  u :", L"  f :",
            L" -u ", L" -x ", L" -i ", L" -d ", L" -f ",
            L"PPF a ", L"PPF c ", L"PPF s ", L"PPF f ",
            L"          PPF"
        };
        for (size_t i = 0; i < sizeof(suppress_patterns)/sizeof(suppress_patterns[0]); ++i) {
            const wchar_t *pat = suppress_patterns[i];
            size_t plen = wcslen(pat);
            if (_wcsnicmp(p, pat, (int)plen) == 0) { free(buf); return NULL; }
            if (wcsstr(buf, pat)) { free(buf); return NULL; }
        }
    }

    // Replaced by centralized translation table `g_translation_rules` above.
    // See g_translation_rules for all mappings and idempotence notes.

    // Apply all rules in a single pass (case-sensitive first, then case-insensitive)
    {
        wchar_t *r = ReplaceMultipleWideSinglePass(buf, g_translation_rules, sizeof(g_translation_rules)/sizeof(g_translation_rules[0]));
        if (r) { free(buf); buf = r; }
    }

    // Run label-preserving replacements (Version : value, etc.) using centralized label pairs
    for (size_t i = 0; i < sizeof(g_label_pairs)/sizeof(g_label_pairs[0]); ++i) {
        buf = ReplaceLabelCI(buf, &bufSize, g_label_pairs[i].en, g_label_pairs[i].es);
    }

    // Log translation (no-op by default unless PPFMANAGER_DEBUG is set)
    DebugLogTranslate(line, buf);

    // Align colon for known key/value lines so colons are vertically aligned in monospaced output
    {
        const int align_width = 15; // desired minimum key column width
        // find first non-space char
        const wchar_t *p = buf;
        while (*p == L' ') p++;
        for (size_t i = 0; i < sizeof(g_label_pairs)/sizeof(g_label_pairs[0]); ++i) {
            const wchar_t *k = g_label_pairs[i].en; // check English label first
            size_t klen = wcslen(k);
            if (wcslen(p) >= klen && _wcsnicmp(p, k, klen) == 0) {
                wchar_t *colon = wcschr((wchar_t*)p, L':');
                if (colon) {
                    // key text is p..colon-1, trim trailing spaces
                    size_t kl = colon - p;
                    while (kl > 0 && p[kl-1] == L' ') kl--;
                    // copy key text
                    wchar_t keytxt[128] = {0};
                    if (kl >= sizeof(keytxt)/sizeof(wchar_t)) kl = sizeof(keytxt)/sizeof(wchar_t) - 1;
                    wcsncpy(keytxt, p, kl);
                    keytxt[kl] = 0;
                    // value begins after colon (skip spaces)
                    const wchar_t *val = colon + 1;
                    while (*val == L' ') val++;
                    int pad = align_width - (int)wcslen(keytxt);
                    if (pad < 1) pad = 1;
                    // build new string: preserve original leading spaces
                    int lead = (int)(p - buf);
                    size_t newlen = wcslen(buf) + pad + 4;
                    wchar_t *nb = (wchar_t*)malloc((newlen + 1) * sizeof(wchar_t));
                    if (!nb) break;
                    nb[0] = 0;
                    for (int s = 0; s < lead; ++s) wcscat(nb, L" ");
                    wcscat(nb, keytxt);
                    for (int s = 0; s < pad; ++s) wcscat(nb, L" ");
                    wcscat(nb, L": ");
                    wcscat(nb, val);
                    free(buf);
                    buf = nb;
                }
                break;
            }
            // also check Spanish variants to handle pre-translated lines
            k = g_label_pairs[i].es; klen = wcslen(k);
            if (wcslen(p) >= klen && _wcsnicmp(p, k, klen) == 0) {
                wchar_t *colon = wcschr((wchar_t*)p, L':');
                if (colon) {
                    size_t kl = colon - p;
                    while (kl > 0 && p[kl-1] == L' ') kl--;
                    wchar_t keytxt[128] = {0};
                    if (kl >= sizeof(keytxt)/sizeof(wchar_t)) kl = sizeof(keytxt)/sizeof(wchar_t) - 1;
                    wcsncpy(keytxt, p, kl);
                    keytxt[kl] = 0;
                    const wchar_t *val = colon + 1; while (*val == L' ') val++;
                    int pad = align_width - (int)wcslen(keytxt); if (pad < 1) pad = 1;
                    int lead = (int)(p - buf);
                    size_t newlen = wcslen(buf) + pad + 4;
                    wchar_t *nb = (wchar_t*)malloc((newlen + 1) * sizeof(wchar_t));
                    if (!nb) break;
                    nb[0] = 0;
                    for (int s = 0; s < lead; ++s) wcscat(nb, L" ");
                    wcscat(nb, keytxt);
                    for (int s = 0; s < pad; ++s) wcscat(nb, L" ");
                    wcscat(nb, L": ");
                    wcscat(nb, val);
                    free(buf);
                    buf = nb;
                }
                break;
            }
        }
    }

    // Suppress console-mode help blocks in GUI: hide usage/commands/examples lines (EN/ES)
    {
        const wchar_t *p = buf;
        while (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n') p++;
        const wchar_t *suppress_patterns[] = {
            L"Usage:", L"Uso:", L"<Commands>", L"<Comandos>", L"<Switches>", L"<Opciones>", L"Examples:", L"Ejemplos:", L"Example:", L"Ejemplo:",
            L"  c :", L"  s :", L"  a :", L"  u :",
            L" -u :", L" -x :", L" -i :", L" -d :", L" -f :"
        };
        for (size_t i = 0; i < sizeof(suppress_patterns)/sizeof(suppress_patterns[0]); ++i) {
            const wchar_t *pat = suppress_patterns[i];
            size_t plen = wcslen(pat);
            if (_wcsnicmp(p, pat, (int)plen) == 0) { free(buf); return NULL; }
            if (wcsstr(buf, pat)) { free(buf); return NULL; }
        }
    }

    return buf;
}

// Helper to redirect stdout/stderr to a temporary file for capturing output.
// IMPORTANT: use _dup/_dup2 instead of freopen to avoid leaving the CRT in a broken state in GUI apps.
typedef struct {
    int fd_out;
    int fd_err;
    int old_fd_out;
    int old_fd_err;
    FILE *tmp_file;
    char temp_filename[MAX_PATH];
    char *buffer;
    size_t buffer_size;
    int last_errno;
    DWORD last_winerr;
    int used_freopen;
} StdoutRedirect;

static void EnsureStdFdsOpenEx(int fd_out, int fd_err) {
    // In GUI apps stdout/stderr fds may be invalid depending on toolchain/runtime.
    // Ensure the specific fds we will touch are backed by a valid OS handle.
    int fds[4] = { fd_out, fd_err, 1, 2 };
    for (int i = 0; i < 4; ++i) {
        int fd = fds[i];
        if (fd < 0) continue;
        errno = 0;
        intptr_t h = _get_osfhandle(fd);
        BOOL need_fix = FALSE;
        if (h == -1) {
            need_fix = TRUE;
        } else {
            // In some GUI+console-subsystem scenarios (e.g. console created then FreeConsole()),
            // CRT fds may still exist but point to an invalid OS handle.
            SetLastError(0);
            DWORD ft = GetFileType((HANDLE)h);
            DWORD gle = GetLastError();
            if (ft == FILE_TYPE_UNKNOWN && gle != NO_ERROR) {
                need_fix = TRUE;
            }
        }
        if (need_fix) {
            int nul = _open("NUL", _O_WRONLY | _O_BINARY);
            if (nul >= 0) {
                _dup2(nul, fd);
                _close(nul);
            }
        }
    }
}

// In GUI mode (no console), ensure stdout/stderr are in a known-good state.
// This avoids intermittent "no output captured" issues when a console was created and then released.
static void EnsureGuiStdioReady(void) {
    int fd_out = _fileno(stdout);
    int fd_err = _fileno(stderr);
    if (fd_out < 0 || fd_err < 0) {
        // Ensure streams exist even if CRT initialized them in a broken state.
        freopen("NUL", "wb", stdout);
        freopen("NUL", "wb", stderr);
    }
    fd_out = _fileno(stdout);
    fd_err = _fileno(stderr);
    if (fd_out < 0) fd_out = 1;
    if (fd_err < 0) fd_err = 2;
    EnsureStdFdsOpenEx(fd_out, fd_err);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    // Keep NUL/stdout in binary mode so capture isn't affected by text translations.
    if (fd_out >= 0) _setmode(fd_out, _O_BINARY);
    if (fd_err >= 0) _setmode(fd_err, _O_BINARY);
}

static int RedirectStdout(StdoutRedirect *redirect) {
    if (!redirect) return 0;
    redirect->buffer = NULL;
    redirect->buffer_size = 0;
    redirect->fd_out = -1;
    redirect->fd_err = -1;
    redirect->old_fd_out = -1;
    redirect->old_fd_err = -1;
    redirect->tmp_file = NULL;
    redirect->temp_filename[0] = 0;
    redirect->last_errno = 0;
    redirect->last_winerr = 0;
    redirect->used_freopen = 0;

    // Determine the actual CRT file descriptors used by stdout/stderr.
    // In some GUI runtimes these may not be 1/2, and assuming 1/2 can cause EINVAL in _dup/_dup2.
    redirect->fd_out = _fileno(stdout);
    redirect->fd_err = _fileno(stderr);
    if (redirect->fd_out < 0) redirect->fd_out = 1;
    if (redirect->fd_err < 0) redirect->fd_err = 2;

    EnsureStdFdsOpenEx(redirect->fd_out, redirect->fd_err);

    // If stdout/stderr fds still don't have backing handles, continue anyway and let freopen fallback try.

    // Prefer unnamed temporary file (auto-deleted) to avoid path/permission issues.
    redirect->tmp_file = tmpfile();
    int temp_fd = -1;
    if (redirect->tmp_file) {
        temp_fd = _fileno(redirect->tmp_file);
        if (temp_fd >= 0) {
            _setmode(temp_fd, _O_BINARY);
        }
    }

    // Fallback to named temp file if tmpfile() fails.
    if (temp_fd < 0) {
        char temp_path[MAX_PATH];
        if (!GetTempPathA(MAX_PATH, temp_path)) { redirect->last_winerr = GetLastError(); return 0; }
        if (!GetTempFileNameA(temp_path, "ppf", 0, redirect->temp_filename)) { redirect->last_winerr = GetLastError(); return 0; }
        // NOTE: do NOT use _O_TEMPORARY here; we may need to reopen this path via freopen() fallback.
        // We'll delete the file manually in RestoreStdout().
        temp_fd = _open(redirect->temp_filename,
                        _O_CREAT | _O_TRUNC | _O_RDWR | _O_BINARY | _O_NOINHERIT,
                        _S_IREAD | _S_IWRITE);
        if (temp_fd < 0) { redirect->last_errno = errno; return 0; }
    }

    redirect->old_fd_out = _dup(redirect->fd_out);
    redirect->old_fd_err = _dup(redirect->fd_err);
    if (redirect->old_fd_out < 0 || redirect->old_fd_err < 0) {
        redirect->last_errno = errno;
        if (redirect->old_fd_out >= 0) _close(redirect->old_fd_out);
        if (redirect->old_fd_err >= 0) _close(redirect->old_fd_err);
        if (!redirect->tmp_file) _close(temp_fd);
        // Try freopen-based fallback below
        goto fallback_freopen;
    }

    // Redirect stdout/stderr to the temp file.
    if (_dup2(temp_fd, redirect->fd_out) != 0 || _dup2(temp_fd, redirect->fd_err) != 0) {
        redirect->last_errno = errno;
        _dup2(redirect->old_fd_out, redirect->fd_out);
        _dup2(redirect->old_fd_err, redirect->fd_err);
        _close(redirect->old_fd_out);
        _close(redirect->old_fd_err);
        redirect->old_fd_out = -1;
        redirect->old_fd_err = -1;
        if (!redirect->tmp_file) _close(temp_fd);
        // Try freopen-based fallback below
        goto fallback_freopen;
    }

    // stdout/stderr now point to the temp file; close our extra fd handle only for the named-temp case.
    if (!redirect->tmp_file) {
        _close(temp_fd);
    }
    // Ensure captured output is written immediately (important when GUI reads it right after execution)
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (redirect->fd_out >= 0) _setmode(redirect->fd_out, _O_BINARY);
    if (redirect->fd_err >= 0) _setmode(redirect->fd_err, _O_BINARY);
    return 1;

fallback_freopen:
    // If we have a named temp filename, freopen stdout/stderr to it.
    // This tends to work even when _dup2 fails with EINVAL in some GUI CRT states.
    if (redirect->temp_filename[0] == 0) {
        char temp_path2[MAX_PATH];
        if (!GetTempPathA(MAX_PATH, temp_path2)) { redirect->last_winerr = GetLastError(); return 0; }
        if (!GetTempFileNameA(temp_path2, "ppf", 0, redirect->temp_filename)) { redirect->last_winerr = GetLastError(); return 0; }
    }
    {
        FILE *fout = freopen(redirect->temp_filename, "wb", stdout);
        FILE *ferr = freopen(redirect->temp_filename, "ab", stderr);
        if (fout && ferr) {
            redirect->used_freopen = 1;
            setvbuf(stdout, NULL, _IONBF, 0);
            setvbuf(stderr, NULL, _IONBF, 0);
            return 1;
        }
        redirect->last_errno = errno;
        return 0;
    }
}

static void RestoreStdout(StdoutRedirect *redirect) {
    if (!redirect) return;
    fflush(stdout);
    fflush(stderr);

    // Restore stdout/stderr fds
    if (redirect->old_fd_out >= 0) {
        _dup2(redirect->old_fd_out, redirect->fd_out >= 0 ? redirect->fd_out : 1);
        _close(redirect->old_fd_out);
        redirect->old_fd_out = -1;
    }
    if (redirect->old_fd_err >= 0) {
        _dup2(redirect->old_fd_err, redirect->fd_err >= 0 ? redirect->fd_err : 2);
        _close(redirect->old_fd_err);
        redirect->old_fd_err = -1;
    }

    // Read the temp file into buffer
    if (redirect->tmp_file) {
        // Ensure OS view of file is up to date before reading (stdout/stderr wrote through a different stream).
        fflush(redirect->tmp_file);
        {
            int tfd = _fileno(redirect->tmp_file);
            if (tfd >= 0) {
                _commit(tfd);
                intptr_t th = _get_osfhandle(tfd);
                if (th != -1) {
                    FlushFileBuffers((HANDLE)th);
                }
            }
        }
        fseek(redirect->tmp_file, 0, SEEK_END);
        long size = ftell(redirect->tmp_file);
        fseek(redirect->tmp_file, 0, SEEK_SET);
        if (size > 0) {
            redirect->buffer = (char*)malloc((size_t)size + 1);
            if (redirect->buffer) {
                redirect->buffer_size = fread(redirect->buffer, 1, (size_t)size, redirect->tmp_file);
                redirect->buffer[redirect->buffer_size] = 0;
            }
        }
        fclose(redirect->tmp_file);
        redirect->tmp_file = NULL;
    } else if (redirect->temp_filename[0]) {
        FILE *f = fopen(redirect->temp_filename, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            fseek(f, 0, SEEK_SET);

            if (size > 0) {
                redirect->buffer = (char*)malloc((size_t)size + 1);
                if (redirect->buffer) {
                    redirect->buffer_size = fread(redirect->buffer, 1, (size_t)size, f);
                    redirect->buffer[redirect->buffer_size] = 0;
                }
            }
            fclose(f);
        }

        // Delete temp file
        DeleteFileA(redirect->temp_filename);
        redirect->temp_filename[0] = 0;
    }
}

// Integrated execution thread: calls MakePPF/ApplyPPF functions directly
static DWORD WINAPI IntegratedExecutionThread(LPVOID lpParam) {
    PROC_THREAD_PARAM *p = (PROC_THREAD_PARAM*)lpParam;
    HWND postTarget = g_hwndMain ? g_hwndMain : GetForegroundWindow();
    int stdout_redirected = 0;
    int lock_acquired = 0;

    enum { MAX_GUI_ARGS = 64 };
    char *argv[MAX_GUI_ARGS];
    wchar_t *wargv[MAX_GUI_ARGS];
    int argc = 0;
    ZeroMemory(argv, sizeof(argv));
    ZeroMemory(wargv, sizeof(wargv));
    
    // Check if another operation is already running
    if (InterlockedCompareExchange(&g_operation_running, 1, 0) != 0) {
        wchar_t *warn = _wcsdup(L"⚠️ Operación ya en curso. Por favor espera a que termine.\r\n\r\n");
        SafePostAllocatedString(postTarget, WM_APPEND_OUTPUT, (WPARAM)p->hEdit, warn);
        free(p);
        return 1;
    }
    lock_acquired = 1;
    
    // Parse command line to extract argc/argv
    wchar_t cmdline[1024];
    wcscpy(cmdline, p->cmdline);
    
    // Parse wide command line into argc/argv using Windows API (respeta comillas)
    int wargc = 0;
    wchar_t **wargv_temp = CommandLineToArgvW(cmdline, &wargc);
    if (wargv_temp && wargc > 0) {
        // Copiar argumentos a nuestros arrays
        for (int i = 0; i < wargc && i < MAX_GUI_ARGS; i++) {
            wargv[i] = _wcsdup(wargv_temp[i]);
            // Convert to ANSI (CP_ACP) - MakePPF/ApplyPPF expect system codepage encoding
            int need = WideCharToMultiByte(CP_ACP, 0, wargv_temp[i], -1, NULL, 0, NULL, NULL);
            if (need > 0) {
                argv[i] = (char*)malloc((size_t)need);
                if (argv[i]) {
                    WideCharToMultiByte(CP_ACP, 0, wargv_temp[i], -1, argv[i], need, NULL, NULL);
                }
            }
            argc++;
        }
        LocalFree(wargv_temp);
    }
    
    // Show command header
    {
        // Strip paths from command line for display
        wchar_t *shortbuf = StripPathsFromCommand(p->cmdline);
        if (!shortbuf) shortbuf = _wcsdup(p->cmdline);
        // Replace displayed program names with PPFManager.exe for a consistent UI
        wchar_t *repl = ReplaceAllWide(shortbuf, L"MakePPF", L"PPFManager.exe");
        if (repl) { free(shortbuf); shortbuf = repl; }
        repl = ReplaceAllWide(shortbuf, L"ApplyPPF", L"PPFManager.exe");
        if (repl) { free(shortbuf); shortbuf = repl; }
        
        wchar_t tmp[2048];
        _snwprintf_s(tmp, 2048, _TRUNCATE, tw("exec"), shortbuf);
        // Separator (post as its own message so it always appears distinctly)
        {
            const wchar_t *sepLine = L"———————————————————————————————————————————————————";
            size_t capSep = wcslen(sepLine) + 3;
            wchar_t *wsep = (wchar_t*)malloc(capSep * sizeof(wchar_t));
            if (wsep) {
                swprintf_s(wsep, capSep, L"%s\r\n", sepLine);
                SafePostAllocatedString(postTarget, WM_APPEND_OUTPUT, (WPARAM)p->hEdit, wsep);
            }
        }
        {
            size_t cap = wcslen(tmp) + 5;
            wchar_t *wbuf = (wchar_t*)malloc(cap * sizeof(wchar_t));
            if (wbuf) {
                swprintf_s(wbuf, cap, L"%s\r\n\r\n", tmp);
                SafePostAllocatedString(postTarget, WM_APPEND_OUTPUT, (WPARAM)p->hEdit, wbuf);
            }
        }
        free(shortbuf);
    }
    
    // Redirect stdout to buffer
    char temp_stdout_path[MAX_PATH] = {0};
    char temp_stderr_path[MAX_PATH] = {0};
    
    // Get temp directory and create secure temp files via Win32 API
    char temp_dir[MAX_PATH];
    if (GetTempPathA(MAX_PATH, temp_dir) > 0) {
        // Create unique temp file names (GetTempFileName creates the file atomically)
        if (GetTempFileNameA(temp_dir, "PPF", 0, temp_stdout_path) && GetTempFileNameA(temp_dir, "PPF", 0, temp_stderr_path)) {
            // Redirect stdout and stderr using freopen; the files already exist and are empty
            FILE *fout = freopen(temp_stdout_path, "w+b", stdout);
            FILE *ferr = freopen(temp_stderr_path, "w+b", stderr);
            if (fout && ferr) {
                stdout_redirected = 1;
                setvbuf(stdout, NULL, _IONBF, 0);
                setvbuf(stderr, NULL, _IONBF, 0);
            } else {
                // If freopen failed, close any partially opened handles and remove files
                if (fout) fclose(fout);
                if (ferr) fclose(ferr);
                DeleteFileA(temp_stdout_path);
                DeleteFileA(temp_stderr_path);
                temp_stdout_path[0] = '\0'; temp_stderr_path[0] = '\0';
            }
        }
    }
    
    if (!stdout_redirected) {
        // Fallback: call directly without capture
        // This is not ideal but better than failing completely
    }
    
    // Determine which command to execute
    int result = 0;
    if (argc > 0) {
        char *exe_name = argv[0];
        
        // CRITICAL: Reset global state before each operation to prevent crashes on repeated executions
        ResetGlobalState();
        
        // Check if it's MakePPF or ApplyPPF
        if (strstr(exe_name, "MakePPF") || strstr(exe_name, "makeppf")) {
            // Check if this is a "show info" operation (contains " s" command)
            int is_show_info = 0;
            if (argc >= 2 && strcmp(argv[1], "s") == 0) {
                is_show_info = 1;
            }
            
            if (!is_show_info) {
                /* Install GUI progress callback so MakePPF can report percent in real-time */
                /* Ensure UI shows 0% immediately */
                PostMessageW(postTarget, WM_CREAR_PROGRESS, (WPARAM)0, 0);
                MakePPF_SetProgressCallback(GuiMakePPFProgress);
            }
            result = MakePPF_Main(argc, argv);
            if (!is_show_info) {
                MakePPF_SetProgressCallback(NULL);
                /* Ensure UI shows 100% on completion */
                PostMessageW(postTarget, WM_CREAR_PROGRESS, (WPARAM)10000, 0);
            }
        } else if (strstr(exe_name, "ApplyPPF") || strstr(exe_name, "applyppf")) {
            /* Install GUI progress callback so ApplyPPF can report percent in real-time */
            /* Ensure UI shows 0% immediately */
            PostMessageW(postTarget, WM_APLICAR_PROGRESS, (WPARAM)0, 0);
            ApplyPPF_SetProgressCallback(GuiApplyProgress);
            result = ApplyPPF_Main(argc, argv);
            ApplyPPF_SetProgressCallback(NULL);
            /* Ensure UI shows 100% on completion */
            PostMessageW(postTarget, WM_APLICAR_PROGRESS, (WPARAM)10000, 0);
        } else {
            printf("Error: unknown command\n");
            result = 1;
        }
    }
    
    // Restore stdout/stderr if redirected
    if (stdout_redirected) {
        fflush(stdout);
        fflush(stderr);
        fclose(stdout);
        fclose(stderr);
        
        // Read captured output
        FILE *read_out = fopen(temp_stdout_path, "rb");
        if (read_out) {
            fseek(read_out, 0, SEEK_END);
            long size64 = ftell(read_out);
            fseek(read_out, 0, SEEK_SET);
            
            if (size64 > 0 && size64 < 10*1024*1024) {
                char *buffer = (char*)malloc((size_t)size64 + 1);
                if (buffer) {
                    size_t nread = fread(buffer, 1, (size_t)size64, read_out);
                    buffer[nread] = 0;
                    
                    // Process line by line
                    char *line_start = buffer;
                    char *line_end = buffer;
                    while (*line_end) {
                        if (*line_end == '\n' || *line_end == '\r') {
                            *line_end = 0;
                            if (line_start < line_end) {
                                // Convert to wide char
                                int wlen = MultiByteToWideChar(CP_UTF8, 0, line_start, -1, NULL, 0);
                                int used_cp = CP_UTF8;
                                if (wlen <= 0) {
                                    wlen = MultiByteToWideChar(CP_ACP, 0, line_start, -1, NULL, 0);
                                    used_cp = CP_ACP;
                                }
                                if (wlen > 0) {
                                    wchar_t *wline = (wchar_t*)malloc(wlen * sizeof(wchar_t));
                                    if (wline) {
                                        MultiByteToWideChar(used_cp, 0, line_start, -1, wline, wlen);
                                        wchar_t *filtered = TranslateConsoleLine(wline);
                                        free(wline);
                                        if (filtered) {
                                            size_t wlen_final = wcslen(filtered) + 3;
                                            wchar_t *wline_nl = (wchar_t*)malloc(wlen_final * sizeof(wchar_t));
                                            if (wline_nl) {
                                                wcscpy_s(wline_nl, wlen_final, filtered);
                                                wcscat_s(wline_nl, wlen_final, L"\r\n");
                                                SafePostAllocatedString(postTarget, WM_APPEND_OUTPUT, (WPARAM)p->hEdit, wline_nl);
                                            }
                                            free(filtered);
                                        }
                                    }
                                }
                            }
                            line_start = line_end + 1;
                            if (*line_end == '\r' && *(line_end + 1) == '\n') {
                                line_end++;
                                line_start++;
                            }
                        }
                        line_end++;
                    }
                    
                    // Last line without newline
                    if (line_start < line_end) {
                        int wlen = MultiByteToWideChar(CP_UTF8, 0, line_start, -1, NULL, 0);
                        int used_cp = CP_UTF8;
                        if (wlen <= 0) {
                            wlen = MultiByteToWideChar(CP_ACP, 0, line_start, -1, NULL, 0);
                            used_cp = CP_ACP;
                        }
                        if (wlen > 0) {
                            wchar_t *wline = (wchar_t*)malloc(wlen * sizeof(wchar_t));
                            if (wline) {
                                MultiByteToWideChar(used_cp, 0, line_start, -1, wline, wlen);
                                wchar_t *filtered = TranslateConsoleLine(wline);
                                free(wline);
                                if (filtered) {
                                    size_t wlen_final = wcslen(filtered) + 3;
                                    wchar_t *wline_nl = (wchar_t*)malloc(wlen_final * sizeof(wchar_t));
                                    if (wline_nl) {
                                        wcscpy_s(wline_nl, wlen_final, filtered);
                                        wcscat_s(wline_nl, wlen_final, L"\r\n");
                                        SafePostAllocatedString(postTarget, WM_APPEND_OUTPUT, (WPARAM)p->hEdit, wline_nl);
                                    }
                                    free(filtered);
                                }
                            }
                        }
                    }
                    
                    free(buffer);
                }
            }
            fclose(read_out);
        }
        
        // Clean up temp files
        DeleteFileA(temp_stdout_path);
        DeleteFileA(temp_stderr_path);
    }
    for (int i = 0; i < argc; ++i) {
        if (argv[i]) free(argv[i]);
        if (wargv[i]) free(wargv[i]);
    }
    free(p);

    if (lock_acquired) {
        InterlockedExchange(&g_operation_running, 0);
        PostMessageW(g_hwndMain ? g_hwndMain : GetForegroundWindow(), WM_ENABLE_BROWSE, (WPARAM)1, 0);
    }

    return result;
}

// Use integrated execution instead of external process
#define ProcessCaptureThread IntegratedExecutionThread


// Return text width in pixels for given font and text (file-scope)
static int GetTextWidthInPixels(HWND hwndRef, HFONT hF, const wchar_t *text) {
    if (!text) return 0;
    HDC hdc = GetDC(hwndRef);
    if (!hdc) return 0;
    HGDIOBJ old = NULL;
    if (hF) old = SelectObject(hdc, hF);
    SIZE sz = {0};
    GetTextExtentPoint32W(hdc, text, (int)wcslen(text), &sz);
    if (old) SelectObject(hdc, old);
    ReleaseDC(hwndRef, hdc);
    return sz.cx;
}

static int GetControlTextWidth(HWND hwndRef, HFONT hF, HWND hCtrl) {
    if (!hCtrl) return 0;
    wchar_t buf[256];
    buf[0] = 0;
    GetWindowTextW(hCtrl, buf, (int)(sizeof(buf) / sizeof(buf[0])));
    return GetTextWidthInPixels(hwndRef, hF, buf);
}

static int GetMaxLabelTextWidth(HWND hwndRef, HFONT hF, HWND *labels, int count) {
    int maxW = 0;
    for (int i = 0; i < count; ++i) {
        if (!labels[i]) continue;
        int w = GetControlTextWidth(hwndRef, hF, labels[i]);
        if (w > maxW) maxW = w;
    }
    return maxW;
}

// Compute button width using the same reference and constants as Apply panel
static int ComputeButtonWidth(HWND hwndRef, HFONT hF, const wchar_t *text, HWND scaleRef) {
    int base = GetTextWidthInPixels(hwndRef, hF, text);
    int margin = ScaleForWindow(scaleRef, 20); // uses same scale reference as Apply
    return base + margin;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // ...existing code...
    static HWND hwndTab = NULL;
    static HWND hCrearPanel = NULL;
    static HWND hCrearLblImg, hCrearEditImg, hCrearBtnImg;
    static HWND hCrearLblMod, hCrearEditMod, hCrearBtnMod;
    static HWND hCrearLblPPF, hCrearEditPPF, hCrearBtnPPF;
    static HWND hCrearLblDIZ, hCrearEditDIZ, hCrearBtnDIZ;
    static HWND hCrearLblDesc, hCrearEditDesc;
    static HWND hCrearChkUndo, hCrearChkValid, hCrearLblTipo, hCrearComboTipo;
    static HWND hCrearBtnCrear, hCrearBtnShow, hCrearBtnAdd, hCrearBtnClear;

    static HWND botones[12] = {0};
    static HWND hCrearOutput;
    static HWND hCrearLblSalida;

    /* Progress helpers are defined at file scope near the top of this file to avoid duplicates. */
    // Aplicar Parche controls
    static HWND hAplicarPanel = NULL;
    static HWND hAplicarLblImg, hAplicarEditImg, hAplicarBtnImg;
    static HWND hAplicarLblPPF, hAplicarEditPPF, hAplicarBtnPPF;
    static HWND hAplicarChkRevert, hAplicarBtnApply, hAplicarBtnClear;
    static HWND hAplicarOutput;
    static HWND hAplicarLblSalida;
    static HWND themedCtrls[24] = {0};
    static HFONT hMonoFont = NULL;
    static int spcBeforeButtons = 8; // extra vertical pixels before buttons (modifiable)
    switch (msg) {

    // Panel subclass procs: forward WM_COMMAND to main window then call original
    case WM_APP + 200: // internal message to install real procs (no-op)
        return 0;
    case WM_APPEND_OUTPUT: {
        HWND hTarget = (HWND)wParam;
        wchar_t *wtext = (wchar_t*)lParam;
        if (IsWindow(hTarget) && wtext) {
            AppendTextToEdit(hTarget, wtext);
        }
        if (wtext) free(wtext);
        return 0;
    }
    case WM_ENABLE_BROWSE: {
        BOOL enable = (wParam != 0);
        // Enable/disable browse buttons in both panels
        if (hCrearBtnImg) EnableWindow(hCrearBtnImg, enable);
        if (hCrearBtnMod) EnableWindow(hCrearBtnMod, enable);
        if (hCrearBtnPPF) EnableWindow(hCrearBtnPPF, enable);
        if (hCrearBtnDIZ) EnableWindow(hCrearBtnDIZ, enable);
        if (hAplicarBtnImg) EnableWindow(hAplicarBtnImg, enable);
        if (hAplicarBtnPPF) EnableWindow(hAplicarBtnPPF, enable);
        return 0;
    }
    case WM_CREAR_PROGRESS: {
        /* wParam: integer 0..10000 representing percent*100 */
        int ipct = (int)wParam;
        if (ipct <= 0) {
            /* reset to zero and show non-interactive bar */
            CrearProgress_ResetToZero();
        } else {
            /* update to provided percent and show non-interactive bar */
            CrearProgress_SetPos(ipct);
        }
        return 0;
    }
    case WM_APLICAR_PROGRESS: {
        /* wParam: integer 0..10000 representing percent*100 */
        int ipct = (int)wParam;
        if (ipct <= 0) {
            /* reset to zero and show non-interactive bar */
            AplicarProgress_ResetToZero();
        } else {
            /* update to provided percent and show non-interactive bar */
            AplicarProgress_SetPos(ipct);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT pt;
        pt.x = (int)(short)LOWORD(lParam);
        pt.y = (int)(short)HIWORD(lParam);
        HWND hw = WindowFromPoint(pt);
        while (hw) {
            if (hw == hCrearOutput || hw == hAplicarOutput) {
                SendMessageW(hw, WM_MOUSEWHEEL, wParam, lParam);
                return 0;
            }
            hw = GetParent(hw);
        }
        break;
    }
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc; GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, g_brBg);
        return 1;
    }
    case WM_CTLCOLORSTATIC: {
        HWND hCtrl = (HWND)lParam;
        HDC hdc = (HDC)wParam;
        if (hCtrl == hwndTab) {
            SetTextColor(hdc, g_clrText);
            SetBkColor(hdc, g_clrBg);
            return (LRESULT)g_brBg;
        }
        if (hCtrl == g_hCrearOutput || hCtrl == g_hAplicarOutput) {
            SetTextColor(hdc, g_clrEditText);
            SetBkColor(hdc, g_clrEditBg);
            return (LRESULT)g_brEditBg;
        }
        SetTextColor(hdc, g_clrText);
        SetBkColor(hdc, g_clrBg);
        return (LRESULT)g_brBg;
    }
    case WM_CTLCOLORBTN: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, g_clrText);
        SetBkColor(hdc, g_clrBg);
        return (LRESULT)g_brBg;
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, g_clrEditText);
        SetBkColor(hdc, g_clrEditBg);
        return (LRESULT)g_brEditBg;
    }

    case WM_CREATE: {
        g_hwndMain = hwnd;
        UpdateThemeBrushes(false);
        SetTitleBarDark(hwnd, false);
        // Fuente Segoe UI (usar tamaño escalado por DPI)
        if (!hFont) {
            LOGFONTW lf = {0};
            int base_h = 13; // altura en px a 96 DPI
            int dpiLocal = GetWindowDPI(hwnd);
            lf.lfHeight = -MulDiv(base_h, dpiLocal, 96);
            wcscpy(lf.lfFaceName, L"Segoe UI variable");
            hFont = CreateFontIndirectW(&lf);
        }
        // Monospaced font for output windows (Consolas preferred, fallback to Courier New)
        if (!hMonoFont) {
            LOGFONTW lfm = {0};
            int base_hm = 13;  // tamaño fuente consolas (px at 96 DPI)
            int dpiLocal = GetWindowDPI(hwnd);
            lfm.lfHeight = -MulDiv(base_hm, dpiLocal, 96);
            wcscpy(lfm.lfFaceName, L"Consolas");
            hMonoFont = CreateFontIndirectW(&lfm);
            if (!hMonoFont) {
                wcscpy(lfm.lfFaceName, L"Courier New");
                hMonoFont = CreateFontIndirectW(&lfm);
            }
        }
        // Menú superior
        HMENU hMenuBar = CreateMenu();
        HMENU hMenuIdioma = CreateMenu();
        HMENU hMenuTema = CreateMenu();
        HMENU hMenuAyuda = CreateMenu();
        AppendMenuW(hMenuIdioma, MF_STRING, 301, T(L"menu_es"));
        AppendMenuW(hMenuIdioma, MF_STRING, 302, T(L"menu_en"));
        AppendMenuW(hMenuTema, MF_STRING, 203, T(L"menu_dark"));
        AppendMenuW(hMenuTema, MF_STRING, 204, T(L"menu_light"));
        AppendMenuW(hMenuAyuda, MF_STRING, 205, T(L"menu_help_show"));
        AppendMenuW(hMenuAyuda, MF_STRING, 206, T(L"menu_about"));
        RECT rcClient;
        GetClientRect(hwnd, &rcClient);
        hwndTab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            0, 0, rcClient.right, rcClient.bottom, hwnd, (HMENU)100, NULL, NULL);
        g_hwndTab = hwndTab;
        oldTabProc = (WNDPROC)SetWindowLongPtrW(hwndTab, GWLP_WNDPROC, (LONG_PTR)TabProc);
        // Añadir pestañas traducidas
        TCITEMW tie;
        tie.mask = TCIF_TEXT;
        tie.iImage = -1;
        tie.pszText = (LPWSTR)T(L"tab_create");
        TabCtrl_InsertItem(hwndTab, 0, &tie);
        tie.pszText = (LPWSTR)T(L"tab_apply");
        TabCtrl_InsertItem(hwndTab, 1, &tie);
        SendMessageW(hwndTab, WM_SETFONT, (WPARAM)hFont, TRUE);


        RECT rcTab;
        GetClientRect(hwndTab, &rcTab);
        TabCtrl_AdjustRect(hwndTab, FALSE, &rcTab);
        hCrearPanel = CreateWindowExW(0, L"STATIC", NULL, WS_CHILD | WS_VISIBLE,
            rcTab.left, rcTab.top, rcTab.right - rcTab.left, rcTab.bottom - rcTab.top, hwndTab, NULL, NULL, NULL);
        g_hCrearPanel = hCrearPanel;
        ApplyTheme(hCrearPanel);
        // Subclass panel to forward WM_COMMAND messages to main window
        oldCrearPanelProc = (WNDPROC)SetWindowLongPtrW(hCrearPanel, GWLP_WNDPROC, (LONG_PTR)CrearPanelProc);
        SendMessageW(hCrearPanel, WM_SETFONT, (WPARAM)hFont, TRUE);
        AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hMenuIdioma, L"Idioma");
        AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hMenuTema, L"Tema");
        AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hMenuAyuda, L"Ayuda");
        SetMenu(hwnd, hMenuBar);
        UpdateLanguageMenuChecks(hMenuBar);
        UpdateThemeMenuChecks(hMenuBar, g_isDark);
        DrawMenuBar(hwnd);
        // alturas ligeramente mayores para centrar texto y dar más respiro visual (escaladas por DPI)
        int y = ScaleForWindow(hwnd, 15);
        int xlbl = ScaleForWindow(hwnd, 26);
        int xbtn = ScaleForWindow(hwnd, 520);
        int wlbl = ScaleForWindow(hwnd, 150);
        int wbtn = ScaleForWindow(hwnd, 426);
        int h = ScaleForWindow(hwnd, 26); // altura de controles edit y labels
        int hBtn = ScaleForWindow(hwnd, 326);
        int hBrowse = ScaleForWindow(hwnd, 26); // altura botones "..."
        int sep = ScaleForWindow(hwnd, 12);
        int spcBeforeButtonsScaled = ScaleForWindow(hwnd, spcBeforeButtons);

        // Make edit fields start closer to the longest label text (both tabs aligned)
        {
            const wchar_t *labelTexts[] = {
                T(L"lbl_img"), T(L"lbl_mod"), T(L"lbl_ppf_dest"), T(L"lbl_diz"), T(L"lbl_desc"),
                T(L"lbl_img_apply"), T(L"lbl_ppf_apply")
            };
            int maxPx = 0;
            for (int i = 0; i < (int)(sizeof(labelTexts)/sizeof(labelTexts[0])); ++i) {
                int w = GetTextWidthInPixels(hwnd, hFont, labelTexts[i]);
                if (w > maxPx) maxPx = w;
            }
            int pad = ScaleForWindow(hwnd, 14);
            int minW = ScaleForWindow(hwnd, 90);
            int maxW = ScaleForWindow(hwnd, 240);
            int desired = maxPx + pad;
            if (desired < minW) desired = minW;
            if (desired > maxW) desired = maxW;
            wlbl = desired;
        }

        int labelToEditGap = ScaleForWindow(hwnd, 8);
        int xedit = xlbl + wlbl + labelToEditGap;
        int editToBtnGap = ScaleForWindow(hwnd, 10);
        int wedit = xbtn - xedit - editToBtnGap;
        if (wedit < ScaleForWindow(hwnd, 120)) wedit = ScaleForWindow(hwnd, 120);
        // Distribución vertical y sin duplicados
        hCrearLblImg = CreateWindowW(L"STATIC", T(L"lbl_img"), WS_CHILD | WS_VISIBLE,
            xlbl, y + ScaleForWindow(hwnd,3), wlbl, h, hCrearPanel, NULL, NULL, NULL);
        hCrearEditImg = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            xedit, y, wedit, h, hCrearPanel, (HMENU)101, NULL, NULL);
        hCrearBtnImg = CreateWindowW(L"BUTTON", L"...", BTN_STYLE,
            xbtn, y, wbtn, hBrowse, hCrearPanel, (HMENU)111, NULL, NULL);
        y += ((h > hBrowse) ? h : hBrowse) + sep;

        hCrearLblMod = CreateWindowW(L"STATIC", T(L"lbl_mod"), WS_CHILD | WS_VISIBLE,
            xlbl, y + ScaleForWindow(hwnd,3), wlbl, h, hCrearPanel, NULL, NULL, NULL);
        hCrearEditMod = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            xedit, y, wedit, h, hCrearPanel, (HMENU)102, NULL, NULL);
        hCrearBtnMod = CreateWindowW(L"BUTTON", L"...", BTN_STYLE,
            xbtn, y, wbtn, hBrowse, hCrearPanel, (HMENU)112, NULL, NULL);
        y += ((h > hBrowse) ? h : hBrowse) + sep;

        hCrearLblPPF = CreateWindowW(L"STATIC", T(L"lbl_ppf_dest"), WS_CHILD | WS_VISIBLE,
            xlbl, y + ScaleForWindow(hwnd,3), wlbl, h, hCrearPanel, NULL, NULL, NULL);
        hCrearEditPPF = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            xedit, y, wedit, h, hCrearPanel, (HMENU)103, NULL, NULL);
        hCrearBtnPPF = CreateWindowW(L"BUTTON", L"...", BTN_STYLE,
            xbtn, y, wbtn, hBrowse, hCrearPanel, (HMENU)113, NULL, NULL);
        y += h + sep + ScaleForWindow(hwnd,10); // espacio entre archivo PPF y file_id.diz

        hCrearLblDIZ = CreateWindowW(L"STATIC", T(L"lbl_diz"), WS_CHILD | WS_VISIBLE,
            xlbl, y + ScaleForWindow(hwnd,3), wlbl, h, hCrearPanel, NULL, NULL, NULL);
        hCrearEditDIZ = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            xedit, y, wedit, h, hCrearPanel, (HMENU)104, NULL, NULL);
        hCrearBtnDIZ = CreateWindowW(L"BUTTON", L"...", BTN_STYLE,
            xbtn, y, wbtn, hBrowse, hCrearPanel, (HMENU)114, NULL, NULL);
        y += h + sep;

        hCrearLblDesc = CreateWindowW(L"STATIC", T(L"lbl_desc"), WS_CHILD | WS_VISIBLE,
            xlbl, y + ScaleForWindow(hwnd,3), wlbl, h, hCrearPanel, NULL, NULL, NULL);
        hCrearEditDesc = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            xedit, y, wedit, h, hCrearPanel, (HMENU)105, NULL, NULL);
        SendMessageW(hCrearEditDesc, EM_LIMITTEXT, (WPARAM)50, 0);
        y += h + sep;

        hCrearChkUndo = CreateWindowW(L"BUTTON", T(L"chk_undo"), CHK_STYLE,
            xlbl, y, ScaleForWindow(hwnd,170), h, hCrearPanel, (HMENU)121, NULL, NULL);
        // reducir ligeramente la separación horizontal entre los checkboxes
        hCrearChkValid = CreateWindowW(L"BUTTON", T(L"chk_valid"), CHK_STYLE,
            xlbl + ScaleForWindow(hwnd,190), y, ScaleForWindow(hwnd,140), h, hCrearPanel, (HMENU)122, NULL, NULL);
        UpdateCheckboxThemes(g_isDark, hCrearChkUndo, hCrearChkValid, hAplicarChkRevert);
        // activar validación por defecto
        SendMessageW(hCrearChkValid, BM_SETCHECK, BST_CHECKED, 0);
        // etiqueta de tipo y combo; en la fila de checkboxes a la derecha (label bajado 3px para alineación visual)
        hCrearLblTipo = CreateWindowW(L"STATIC", T(L"lbl_tipo"), WS_CHILD | WS_VISIBLE,
            xlbl + ScaleForWindow(hwnd,350), y + ScaleForWindow(hwnd,3), ScaleForWindow(hwnd,50), h, hCrearPanel, NULL, NULL, NULL);
        hCrearComboTipo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            xlbl + ScaleForWindow(hwnd,405), y, ScaleForWindow(hwnd,70), h * 6, hCrearPanel, (HMENU)123, NULL, NULL);
        SendMessageW(hCrearComboTipo, CB_RESETCONTENT, 0, 0);
        SendMessageW(hCrearComboTipo, CB_ADDSTRING, 0, (LPARAM)L"BIN");
        SendMessageW(hCrearComboTipo, CB_ADDSTRING, 0, (LPARAM)L"GI");
        SendMessageW(hCrearComboTipo, CB_ADDSTRING, 0, (LPARAM)L"ISO");
        SendMessageW(hCrearComboTipo, CB_SETCURSEL, 0, 0);
        oldComboProc = (WNDPROC)SetWindowLongPtrW(hCrearComboTipo, GWLP_WNDPROC, (LONG_PTR)ComboProc);
        y += h + sep + spcBeforeButtonsScaled; // pequeño espacio antes de los botones

        hCrearBtnCrear = CreateWindowW(L"BUTTON", T(L"btn_create"), BTN_STYLE,
            xlbl, y, ScaleForWindow(hwnd,120), hBtn, hCrearPanel, (HMENU)131, NULL, NULL);
        hCrearBtnShow = CreateWindowW(L"BUTTON", T(L"btn_show"), BTN_STYLE,
            xlbl + ScaleForWindow(hwnd,130), y, ScaleForWindow(hwnd,130), hBtn, hCrearPanel, (HMENU)132, NULL, NULL);
        hCrearBtnAdd = CreateWindowW(L"BUTTON", T(L"btn_add"), BTN_STYLE,
            xlbl + ScaleForWindow(hwnd,270), y, ScaleForWindow(hwnd,150), hBtn, hCrearPanel, (HMENU)133, NULL, NULL);
        hCrearBtnClear = CreateWindowW(L"BUTTON", T(L"btn_clear"), BTN_STYLE,
            xlbl + ScaleForWindow(hwnd,430), y, ScaleForWindow(hwnd,120), hBtn, hCrearPanel, (HMENU)134, NULL, NULL);
        
        // Ajustar ancho de botones al texto — usar la misma lógica exacta que en la pestaña Aplicar
        int btnMargin = 20; // margen fijo (misma que en aplicar)
        int btnX = xlbl;
        
        wchar_t btnTxt[128];
        GetWindowTextW(hCrearBtnCrear, btnTxt, 128);
        int btnW = GetTextWidthInPixels(hCrearPanel, hFont, btnTxt) + btnMargin;
        MoveWindow(hCrearBtnCrear, btnX, y, btnW, hBtn, TRUE);
        btnX += btnW + 10;
        
        GetWindowTextW(hCrearBtnShow, btnTxt, 128);
        btnW = GetTextWidthInPixels(hCrearPanel, hFont, btnTxt) + btnMargin;
        MoveWindow(hCrearBtnShow, btnX, y, btnW, hBtn, TRUE);
        btnX += btnW + 10;
        
        GetWindowTextW(hCrearBtnAdd, btnTxt, 128);
        btnW = GetTextWidthInPixels(hCrearPanel, hFont, btnTxt) + btnMargin;
        MoveWindow(hCrearBtnAdd, btnX, y, btnW, hBtn, TRUE);
        btnX += btnW + 10;
        
        GetWindowTextW(hCrearBtnClear, btnTxt, 128);
        btnW = GetTextWidthInPixels(hCrearPanel, hFont, btnTxt) + btnMargin;
        MoveWindow(hCrearBtnClear, btnX, y, btnW, hBtn, TRUE);

        y += hBtn + sep + spcBeforeButtonsScaled;

        hCrearOutput = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            xlbl, y, ScaleForWindow(hwnd,570), ScaleForWindow(hwnd,120), hCrearPanel, (HMENU)140, NULL, NULL);
        g_hCrearOutput = hCrearOutput;

        ShowWindow(hCrearPanel, SW_SHOW);

        hAplicarPanel = CreateWindowExW(0, L"STATIC", NULL, WS_CHILD | WS_VISIBLE,
            rcTab.left, rcTab.top, rcTab.right - rcTab.left, rcTab.bottom - rcTab.top, hwndTab, NULL, NULL, NULL);
        // Subclass apply panel as well
        g_hAplicarPanel = hAplicarPanel;
        ApplyTheme(hAplicarPanel);
        oldAplicarPanelProc = (WNDPROC)SetWindowLongPtrW(hAplicarPanel, GWLP_WNDPROC, (LONG_PTR)AplicarPanelProc);
        SendMessageW(hAplicarPanel, WM_SETFONT, (WPARAM)hFont, TRUE);
        int ay = ScaleForWindow(hwnd,10);
        hAplicarLblImg = CreateWindowW(L"STATIC", T(L"lbl_img_apply"), WS_CHILD | WS_VISIBLE,
            xlbl, ay + ScaleForWindow(hwnd,3), wlbl, h, hAplicarPanel, NULL, NULL, NULL);
        hAplicarEditImg = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            xedit, ay, wedit, h, hAplicarPanel, (HMENU)201, NULL, NULL);
        hAplicarBtnImg = CreateWindowW(L"BUTTON", L"...", BTN_STYLE,
            xbtn, ay, wbtn, hBrowse, hAplicarPanel, (HMENU)211, NULL, NULL);
        ay += ((h > hBrowse) ? h : hBrowse) + sep;

        hAplicarLblPPF = CreateWindowW(L"STATIC", T(L"lbl_ppf_apply"), WS_CHILD | WS_VISIBLE,
            xlbl, ay + ScaleForWindow(hwnd,3), wlbl, h, hAplicarPanel, NULL, NULL, NULL);
        hAplicarEditPPF = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            xedit, ay, wedit, h, hAplicarPanel, (HMENU)402, NULL, NULL);
        hAplicarBtnPPF = CreateWindowW(L"BUTTON", L"...", BTN_STYLE,
            xbtn, ay, wbtn, hBrowse, hAplicarPanel, (HMENU)212, NULL, NULL);
        ay += ((h > hBrowse) ? h : hBrowse) + sep;

        hAplicarChkRevert = CreateWindowW(L"BUTTON", T(L"chk_revert"), CHK_STYLE,
            xlbl, ay, ScaleForWindow(hwnd,200), h, hAplicarPanel, (HMENU)221, NULL, NULL);
        UpdateCheckboxThemes(g_isDark, hCrearChkUndo, hCrearChkValid, hAplicarChkRevert);
        ay += h + sep + spcBeforeButtonsScaled; // pequeño espacio antes de los botones
        hAplicarBtnApply = CreateWindowW(L"BUTTON", T(L"btn_apply"), BTN_STYLE,
            xlbl, ay, ScaleForWindow(hwnd,120), hBtn, hAplicarPanel, (HMENU)231, NULL, NULL);
        hAplicarBtnClear = CreateWindowW(L"BUTTON", T(L"btn_clear_apply"), BTN_STYLE,
            xlbl + ScaleForWindow(hwnd,130), ay, ScaleForWindow(hwnd,120), hBtn, hAplicarPanel, (HMENU)232, NULL, NULL);
        
        // Ajustar ancho de botones al texto
        wchar_t btnTxt2[128];
        int btnX2 = xlbl;
        
        GetWindowTextW(hAplicarBtnApply, btnTxt2, 128);
        int btnW2 = ComputeButtonWidth(hAplicarPanel, hFont, btnTxt2, hAplicarPanel);
        MoveWindow(hAplicarBtnApply, btnX2, ay, btnW2, hBtn, TRUE);
        btnX2 += btnW2 + ScaleForWindow(hwnd,10);
        
        GetWindowTextW(hAplicarBtnClear, btnTxt2, 128);
        btnW2 = ComputeButtonWidth(hAplicarPanel, hFont, btnTxt2, hAplicarPanel);
        MoveWindow(hAplicarBtnClear, btnX2, ay, btnW2, hBtn, TRUE);
        ay += hBtn + sep + spcBeforeButtonsScaled;

        hAplicarLblSalida = CreateWindowW(L"STATIC", T(L"lbl_salida_apply"), WS_CHILD | WS_VISIBLE,
            xlbl, ay, ScaleForWindow(hwnd,60), h, hAplicarPanel, NULL, NULL, NULL);
        ay += h + ScaleForWindow(hwnd,4);
        hAplicarOutput = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            xlbl, ay, ScaleForWindow(hwnd,570), ScaleForWindow(hwnd,120), hAplicarPanel, (HMENU)240, NULL, NULL);
        g_hAplicarOutput = hAplicarOutput;

        // Ocultar panel aplicar por defecto
        ShowWindow(hAplicarPanel, SW_HIDE);

        // Asegurar fuente en todos los controles nuevos
        HWND aplicarControles[] = {hAplicarLblImg, hAplicarEditImg, hAplicarBtnImg, hAplicarLblPPF, hAplicarEditPPF, hAplicarBtnPPF,
            hAplicarChkRevert, hAplicarBtnApply, hAplicarBtnClear, hAplicarLblSalida, hAplicarOutput};
        for (size_t i = 0; i < sizeof(aplicarControles)/sizeof(HWND); ++i) {
            SendMessageW(aplicarControles[i], WM_SETFONT, (WPARAM)hFont, TRUE);
            ApplyTheme(aplicarControles[i]);
        }
        HWND crearControles[] = {hCrearLblImg, hCrearEditImg, hCrearBtnImg, hCrearLblMod, hCrearEditMod, hCrearBtnMod,
            hCrearLblPPF, hCrearEditPPF, hCrearBtnPPF, hCrearLblDIZ, hCrearEditDIZ, hCrearBtnDIZ, hCrearLblDesc, hCrearEditDesc,
            hCrearChkUndo, hCrearChkValid, hCrearLblTipo, hCrearComboTipo, hCrearBtnCrear, hCrearBtnShow, hCrearBtnAdd, hCrearBtnClear, hCrearOutput};
        for (size_t i = 0; i < sizeof(crearControles)/sizeof(HWND); ++i) {
            SendMessageW(crearControles[i], WM_SETFONT, (WPARAM)hFont, TRUE);
            ApplyTheme(crearControles[i]);
        }
        botones[0] = hCrearBtnImg; botones[1] = hCrearBtnMod; botones[2] = hCrearBtnPPF; botones[3] = hCrearBtnDIZ;
        botones[4] = hCrearBtnCrear; botones[5] = hCrearBtnShow; botones[6] = hCrearBtnAdd; botones[7] = hCrearBtnClear;
        botones[8] = hAplicarBtnImg; botones[9] = hAplicarBtnPPF; botones[10] = hAplicarBtnApply; botones[11] = hAplicarBtnClear;
        // controls to apply SetWindowTheme (scrollbars/dropdowns in dark mode)
        themedCtrls[0] = hCrearEditImg; themedCtrls[1] = hCrearEditMod; themedCtrls[2] = hCrearEditPPF; themedCtrls[3] = hCrearEditDIZ;
        themedCtrls[4] = hCrearEditDesc; themedCtrls[5] = hCrearComboTipo; themedCtrls[6] = hCrearOutput;
        themedCtrls[7] = hAplicarEditImg; themedCtrls[8] = hAplicarEditPPF; themedCtrls[9] = hAplicarOutput;
        themedCtrls[10] = hAplicarChkRevert; themedCtrls[11] = hCrearChkUndo; themedCtrls[12] = hCrearChkValid;
        // Use monospaced font for output windows to preserve column alignment
        if (hMonoFont) {
            SendMessageW(hCrearOutput, WM_SETFONT, (WPARAM)hMonoFont, TRUE);
            SendMessageW(hAplicarOutput, WM_SETFONT, (WPARAM)hMonoFont, TRUE);
        }
        // Load saved settings (paths, checkboxes, combo) if present — now that all controls exist
        LoadSettings(hCrearEditImg, hCrearEditMod, hCrearEditPPF, hCrearEditDIZ, hCrearEditDesc, hCrearChkUndo, hCrearChkValid, hCrearComboTipo,
                 hAplicarEditImg, hAplicarEditPPF, hAplicarChkRevert);
        // Apply theme based on saved preference
        ApplyCurrentTheme(g_themePref == 1, hwnd, hwndTab, hCrearPanel, hAplicarPanel, hCrearChkUndo, hCrearChkValid, hAplicarChkRevert, hMenuBar);
        UpdateButtonThemes(g_isDark, botones, 12);
        UpdateControlThemes(g_isDark, themedCtrls, 13);
        // default tab sizing (owner-draw removed)
        // Traducción inicial de la UI
        TranslateUI(hwndTab, hCrearPanel, hAplicarPanel,
            hCrearLblImg, hCrearLblMod, hCrearLblPPF, hCrearLblDIZ, hCrearLblDesc,
            hCrearChkUndo, hCrearChkValid, hCrearLblTipo, hCrearComboTipo, hCrearBtnCrear, hCrearBtnShow, hCrearBtnAdd, hCrearBtnClear, NULL,
            hAplicarLblImg, hAplicarLblPPF, hAplicarChkRevert, hAplicarBtnApply, hAplicarBtnClear, hAplicarLblSalida,
            hMenuBar, hMenuIdioma, hMenuTema, hMenuAyuda);
        // Ensure menu checks reflect loaded language/theme
        UpdateLanguageMenuChecks(hMenuBar);
        UpdateThemeMenuChecks(hMenuBar, g_isDark);
        DrawMenuBar(hwnd);
        break;
    }
    case WM_SIZE: {
        if (hwndTab && hCrearPanel) {
            RECT rcClient;
            GetClientRect(hwnd, &rcClient);
            int margin = ScaleForWindow(hwnd, 6);
            MoveWindow(hwndTab, margin, margin, rcClient.right - (margin * 2), rcClient.bottom - (margin * 2), TRUE);

            RECT rcTab;
            GetClientRect(hwndTab, &rcTab);
            TabCtrl_AdjustRect(hwndTab, FALSE, &rcTab);
            MoveWindow(hCrearPanel, rcTab.left, rcTab.top, rcTab.right - rcTab.left, rcTab.bottom - rcTab.top, TRUE);
            if (hAplicarPanel) {
                MoveWindow(hAplicarPanel, rcTab.left, rcTab.top, rcTab.right - rcTab.left, rcTab.bottom - rcTab.top, TRUE);
            }

            // Recalcular posiciones internas de los controles dentro de hCrearPanel
            RECT rcPanel;
            GetClientRect(hCrearPanel, &rcPanel);
            int panelW = rcPanel.right;
            /* Use DPI-scaled layout metrics so controls remain usable at high DPI
               Scale values with the panel window so they follow its DPI context */
            int marginRight = ScaleForWindow(hCrearPanel, 20);
            int xlbl_local = ScaleForWindow(hCrearPanel, 20);
            int gap = ScaleForWindow(hCrearPanel, 7);
            int wbtn_local = ScaleForWindow(hCrearPanel, 28); // ancho botones examinar
            int h_browse = ScaleForWindow(hCrearPanel, 22); // altura botones examinar
            int labelToEditGap = ScaleForWindow(hCrearPanel, 0);
            // Compute the label width from the actual longest label text (across both tabs) so edit fields start closer
            int wlbl_local = ScaleForWindow(hCrearPanel, 150);
            {
                HWND labels[] = {
                    hCrearLblImg, hCrearLblMod, hCrearLblPPF, hCrearLblDIZ, hCrearLblDesc,
                    hAplicarLblImg, hAplicarLblPPF
                };
                int maxPx = GetMaxLabelTextWidth(hCrearPanel, hFont, labels, (int)(sizeof(labels)/sizeof(labels[0])));
                int pad = ScaleForWindow(hCrearPanel, 14);
                int minW = ScaleForWindow(hCrearPanel, 90);
                int maxW = ScaleForWindow(hCrearPanel, 240);
                int desired = maxPx + pad;
                if (desired < minW) desired = minW;
                if (desired > maxW) desired = maxW;
                wlbl_local = desired;
            }
            int xedit_local = xlbl_local + wlbl_local + labelToEditGap;
            int y_local = ScaleForWindow(hCrearPanel, 10);
            int h_local = ScaleForWindow(hCrearPanel, 22); // altura campos superiores de texto, imagen original...
            int h_action = ScaleForWindow(hCrearPanel, 24); // Botones superiores agregar imagenes...

            // helper macro-like local motions
            if (hCrearLblImg && hCrearEditImg && hCrearBtnImg) {
                int xbtn = panelW - marginRight - wbtn_local;
                int wedit = xbtn - xedit_local - gap;
                MoveWindow(hCrearLblImg, xlbl_local, y_local + 3, wlbl_local, h_local, TRUE);
                MoveWindow(hCrearEditImg, xedit_local, y_local, wedit, h_local, TRUE);
                MoveWindow(hCrearBtnImg, xbtn, y_local, wbtn_local, h_browse, TRUE);
                y_local += h_local + gap;
            }
            
            if (hCrearLblMod && hCrearEditMod && hCrearBtnMod) {
                int xbtn = panelW - marginRight - wbtn_local;
                int wedit = xbtn - xedit_local - gap;
                MoveWindow(hCrearLblMod, xlbl_local, y_local + 3, wlbl_local, h_local, TRUE);
                MoveWindow(hCrearEditMod, xedit_local, y_local, wedit, h_local, TRUE);
                MoveWindow(hCrearBtnMod, xbtn, y_local, wbtn_local, h_browse, TRUE);
                y_local += h_local + gap;
            }
            if (hCrearLblPPF && hCrearEditPPF && hCrearBtnPPF) {
                int xbtn = panelW - marginRight - wbtn_local;
                int wedit = xbtn - xedit_local - gap;
                MoveWindow(hCrearLblPPF, xlbl_local, y_local + 3, wlbl_local, h_local, TRUE);
                MoveWindow(hCrearEditPPF, xedit_local, y_local, wedit, h_local, TRUE);
                MoveWindow(hCrearBtnPPF, xbtn, y_local, wbtn_local, h_browse, TRUE);
                y_local += h_local + gap + 10; // espacio entre archivo PPF y file_id.diz
            }
            if (hCrearLblDIZ && hCrearEditDIZ && hCrearBtnDIZ) {
                int xbtn = panelW - marginRight - wbtn_local;
                int wedit = xbtn - xedit_local - gap;
                MoveWindow(hCrearLblDIZ, xlbl_local, y_local + 3, wlbl_local, h_local, TRUE);
                MoveWindow(hCrearEditDIZ, xedit_local, y_local, wedit, h_local, TRUE);
                MoveWindow(hCrearBtnDIZ, xbtn, y_local, wbtn_local, h_browse, TRUE);
                y_local += h_local + gap;
            }
            if (hCrearLblDesc && hCrearEditDesc) {
                int wedit = panelW - xedit_local - marginRight;
                MoveWindow(hCrearLblDesc, xlbl_local, y_local + 3, wlbl_local, h_local, TRUE);
                MoveWindow(hCrearEditDesc, xedit_local, y_local, wedit, h_local, TRUE);
                y_local += h_local + gap;
            }
            if (hCrearChkUndo && hCrearChkValid && hCrearComboTipo) {
                /* Place the BIN/GI combo directly after the 'Activar validación' checkbox
                   and use DPI-scaled widths so nothing gets clipped. */
                int wcombo = ScaleForWindow(hCrearPanel, 70);
                int chk_undo_w = ScaleForWindow(hCrearPanel, 170);
                int chk_valid_w = ScaleForWindow(hCrearPanel, 140);
                int chk_gap = ScaleForWindow(hCrearPanel, 20); /* gap between checkboxes */

                /* place checkboxes starting at left */
                MoveWindow(hCrearChkUndo, xlbl_local, y_local, chk_undo_w, h_local, TRUE);
                MoveWindow(hCrearChkValid, xlbl_local + chk_undo_w + chk_gap, y_local, chk_valid_w, h_local, TRUE);

                /* Position the label and combo using fixed (scaled) offsets so they align like the original layout */
                int lbl_left = xlbl_local + ScaleForWindow(hCrearPanel, 350);
                int combo_left = xlbl_local + ScaleForWindow(hCrearPanel, 405);
                MoveWindow(hCrearLblTipo, lbl_left, y_local + 3, ScaleForWindow(hCrearPanel,50), h_local, TRUE);
                MoveWindow(hCrearComboTipo, combo_left, y_local, wcombo, h_local * 6, TRUE);

                /* Create or move Create-tab progress control placed immediately under the checkbox row */
                {
                    int progW = ScaleForWindow(hCrearPanel, 475);
                    int progH = ScaleForWindow(hCrearPanel, 12);
                    RECT rcCombo;
                    GetWindowRect(hCrearComboTipo, &rcCombo);
                    MapWindowPoints(NULL, hCrearPanel, (LPPOINT)&rcCombo, 2);
                    int progX = xlbl_local; /* align left with labels for a tidy look */
                    /* clamp width to fit inside panel */
                    if (progX + progW > panelW - marginRight) progW = panelW - marginRight - progX;
                    if (progW < ScaleForWindow(hCrearPanel, 40)) progW = ScaleForWindow(hCrearPanel, 40); /* minimal width */
                    /* espacio superior de barra progreso y linea superior */
                    int progY = y_local + h_local + ScaleForWindow(hCrearPanel, 6);
                    if (!g_hCrearTopProgress) {
                        g_hCrearTopProgress = CreateWindowExW(0, L"msctls_progress32", NULL, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                            progX, progY, progW, progH, hCrearPanel, (HMENU)151, NULL, NULL);
                        if (g_hCrearTopProgress) {
                            SendMessageW(g_hCrearTopProgress, PBM_SETRANGE32, 0, 10000);
                            SendMessageW(g_hCrearTopProgress, PBM_SETSTEP, 1, 0);
                            SendMessageW(g_hCrearTopProgress, PBM_SETPOS, 0, 0);
                            SetWindowTheme(g_hCrearTopProgress, L"Explorer", L"Explorer");
                            /* Keep control disabled so it does not capture mouse clicks in case of overlap */
                            EnableWindow(g_hCrearTopProgress, FALSE);
                            ShowWindow(g_hCrearTopProgress, SW_SHOWNOACTIVATE);
                            SetWindowPos(g_hCrearTopProgress, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                        } else {
                            /* creation failed: log to output edit */
                            wchar_t *err = _wcsdup(L"Warning: failed to create g_hCrearTopProgress\r\n");
                            if (err) SafePostAllocatedString(g_hwndMain ? g_hwndMain : GetForegroundWindow(), WM_APPEND_OUTPUT, (WPARAM)g_hCrearOutput, err);
                        }
                    } else {
                        MoveWindow(g_hCrearTopProgress, progX, progY, progW, progH, TRUE);
                        /* ensure it does not capture clicks */
                        ShowWindow(g_hCrearTopProgress, SW_SHOWNOACTIVATE);
                        EnableWindow(g_hCrearTopProgress, FALSE);
                        SetWindowPos(g_hCrearTopProgress, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                    }
                }

                /* Add spacing equal to checkbox height + progress height + small gap to separate from buttons */
                {
                    int prog_h_local = ScaleForWindow(hCrearPanel, 14);
                    int gap_after = ScaleForWindow(hCrearPanel, 10); // espacio inferior de la barra de progreso
                    /* advance past checkbox row + progress + gap */
                    y_local += h_local + prog_h_local + gap_after;
                }
            }
            if (hCrearBtnCrear && hCrearBtnShow && hCrearBtnAdd && hCrearBtnClear) {
                // Ajustar ancho de botones al texto — usar exactamente la misma referencia y constantes que en la pestaña Aplicar
                int btnX = xlbl_local;
                wchar_t btnTxt[128];
                HWND hwndRefForMeasure = hAplicarPanel ? hAplicarPanel : hCrearPanel;

                GetWindowTextW(hCrearBtnCrear, btnTxt, 128);
                int btnW = ComputeButtonWidth(hwndRefForMeasure, hFont, btnTxt, hAplicarPanel ? hAplicarPanel : hCrearPanel);
                MoveWindow(hCrearBtnCrear, btnX, y_local, btnW, h_action, TRUE);
                btnX += btnW + ScaleForWindow(hwnd,10);

                GetWindowTextW(hCrearBtnShow, btnTxt, 128);
                btnW = ComputeButtonWidth(hwndRefForMeasure, hFont, btnTxt, hAplicarPanel ? hAplicarPanel : hCrearPanel);
                MoveWindow(hCrearBtnShow, btnX, y_local, btnW, h_action, TRUE);
                btnX += btnW + ScaleForWindow(hwnd,10);

                GetWindowTextW(hCrearBtnAdd, btnTxt, 128);
                btnW = ComputeButtonWidth(hwndRefForMeasure, hFont, btnTxt, hAplicarPanel ? hAplicarPanel : hCrearPanel);
                MoveWindow(hCrearBtnAdd, btnX, y_local, btnW, h_action, TRUE);
                btnX += btnW + ScaleForWindow(hwnd,10);

                GetWindowTextW(hCrearBtnClear, btnTxt, 128);
                btnW = ComputeButtonWidth(hwndRefForMeasure, hFont, btnTxt, hAplicarPanel ? hAplicarPanel : hCrearPanel);
                MoveWindow(hCrearBtnClear, btnX, y_local, btnW, h_action, TRUE);
                y_local += h_action + gap;
            }
            if (hCrearLblSalida) {
                MoveWindow(hCrearLblSalida, xlbl_local, y_local, 60, h_local, TRUE);
                y_local += h_local + ScaleForWindow(hCrearPanel, 4);
            }
            if (hCrearOutput) {
                int outW = panelW - xlbl_local - marginRight;
                int outH = rcPanel.bottom - y_local - ScaleForWindow(hCrearPanel, 10); // expandir hasta abajo con margen
                MoveWindow(hCrearOutput, xlbl_local, y_local, outW, outH, TRUE);
            }

            // layout for 'Aplicar Parche' panel controls (same alignment as Crear Parche)
            if (hAplicarPanel) {
                RECT rcAplicar;
                GetClientRect(hAplicarPanel, &rcAplicar);
                int apanelW = rcAplicar.right;
                int axlbl = xlbl_local;
                int axedit = xedit_local;
                int awbtn = wbtn_local;
                int ay_local = ScaleForWindow(hAplicarPanel, 10);
                // Imagen original
                if (hAplicarLblImg && hAplicarEditImg && hAplicarBtnImg) {
                    int axbtn = apanelW - marginRight - awbtn;
                    int awedit = axbtn - axedit - gap;
                    MoveWindow(hAplicarLblImg, axlbl, ay_local + 3, wlbl_local, h_local, TRUE);
                    MoveWindow(hAplicarEditImg, axedit, ay_local, awedit, h_local, TRUE);
                    MoveWindow(hAplicarBtnImg, axbtn, ay_local, awbtn, h_browse, TRUE);
                    ay_local += h_local + gap;
                }
                // Archivo PPF
                if (hAplicarLblPPF && hAplicarEditPPF && hAplicarBtnPPF) {
                    int axbtn = apanelW - marginRight - awbtn;
                    int awedit = axbtn - axedit - gap;
                    MoveWindow(hAplicarLblPPF, axlbl, ay_local + 3, wlbl_local, h_local, TRUE);
                    MoveWindow(hAplicarEditPPF, axedit, ay_local, awedit, h_local, TRUE);
                    MoveWindow(hAplicarBtnPPF, axbtn, ay_local, awbtn, h_browse, TRUE);
                    ay_local += h_local + gap;
                }
                // Botones y checkbox (checkbox on one line, buttons on next)
                if (hAplicarChkRevert && hAplicarBtnApply && hAplicarBtnClear) {
                    wchar_t chkTxtA[128] = {0};
                    GetWindowTextW(hAplicarChkRevert, chkTxtA, 128);
                    int revert_w = GetTextWidthInPixels(hAplicarPanel, hFont, chkTxtA) + ScaleForWindow(hAplicarPanel, 24);
                    if (revert_w < ScaleForWindow(hAplicarPanel, 120)) revert_w = ScaleForWindow(hAplicarPanel, 120);
                    MoveWindow(hAplicarChkRevert, axlbl, ay_local, revert_w, h_local, TRUE);
                    ay_local += h_local + gap + spcBeforeButtons;

                    // Create or move Apply-tab progress control placed immediately under the checkbox
                    {
                        int progW = ScaleForWindow(hAplicarPanel, 475);
                        int progH = ScaleForWindow(hAplicarPanel, 12);
                        int progX = axlbl; /* align left with labels for a tidy look */
                        /* clamp width to fit inside panel */
                        if (progX + progW > apanelW - marginRight) progW = apanelW - marginRight - progX;
                        if (progW < ScaleForWindow(hAplicarPanel, 40)) progW = ScaleForWindow(hAplicarPanel, 40); /* minimal width */
                        /* espacio superior de barra progreso y linea superior */
                        int progY = ay_local - ScaleForWindow(hAplicarPanel, 4);
                        if (!g_hAplicarTopProgress) {
                            g_hAplicarTopProgress = CreateWindowExW(0, L"msctls_progress32", NULL, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                progX, progY, progW, progH, hAplicarPanel, (HMENU)152, NULL, NULL);
                            if (g_hAplicarTopProgress) {
                                SendMessageW(g_hAplicarTopProgress, PBM_SETRANGE32, 0, 10000);
                                SendMessageW(g_hAplicarTopProgress, PBM_SETSTEP, 1, 0);
                                SendMessageW(g_hAplicarTopProgress, PBM_SETPOS, 0, 0);
                                SetWindowTheme(g_hAplicarTopProgress, L"Explorer", L"Explorer");
                                /* Keep control disabled so it does not capture mouse clicks in case of overlap */
                                EnableWindow(g_hAplicarTopProgress, FALSE);
                                ShowWindow(g_hAplicarTopProgress, SW_SHOWNOACTIVATE);
                                SetWindowPos(g_hAplicarTopProgress, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                            } else {
                                /* creation failed: log to output edit */
                                wchar_t *err = _wcsdup(L"Warning: failed to create g_hAplicarTopProgress\r\n");
                                if (err) SafePostAllocatedString(g_hwndMain ? g_hwndMain : GetForegroundWindow(), WM_APPEND_OUTPUT, (WPARAM)hAplicarOutput, err);
                            }
                        } else {
                            MoveWindow(g_hAplicarTopProgress, progX, progY, progW, progH, TRUE);
                            /* ensure it does not capture clicks */
                            ShowWindow(g_hAplicarTopProgress, SW_SHOWNOACTIVATE);
                            EnableWindow(g_hAplicarTopProgress, FALSE);
                            SetWindowPos(g_hAplicarTopProgress, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                        }
                    }

                    /* Add spacing equal to progress height + small gap to separate from buttons */
                    {
                        int prog_h_local = ScaleForWindow(hAplicarPanel, 14);
                        int gap_after = ScaleForWindow(hAplicarPanel, 10);
                        /* advance past progress + gap */
                        ay_local += prog_h_local + gap_after;
                    }

                    // Ajustar ancho de botones al texto
                    int btnMargin2 = ScaleForWindow(hAplicarPanel, 20);
                    int btnX2 = axlbl;
                    wchar_t btnTxt2[128];

                    GetWindowTextW(hAplicarBtnApply, btnTxt2, 128);
                    int btnW2 = GetTextWidthInPixels(hAplicarPanel, hFont, btnTxt2) + btnMargin2;
                    MoveWindow(hAplicarBtnApply, btnX2, ay_local, btnW2, h_action, TRUE);
                    btnX2 += btnW2 + ScaleForWindow(hAplicarPanel, 10);

                    GetWindowTextW(hAplicarBtnClear, btnTxt2, 128);
                    btnW2 = GetTextWidthInPixels(hAplicarPanel, hFont, btnTxt2) + btnMargin2;
                    MoveWindow(hAplicarBtnClear, btnX2, ay_local, btnW2, h_action, TRUE);
                    ay_local += h_action + gap;
                }
                if (hAplicarLblSalida) {
                    MoveWindow(hAplicarLblSalida, axlbl, ay_local, ScaleForWindow(hAplicarPanel,60), h_local, TRUE);
                    ay_local += h_local + ScaleForWindow(hAplicarPanel,4);
                }
                if (hAplicarOutput) {
                    int aoutW = apanelW - axlbl - marginRight;
                    int aoutH = rcAplicar.bottom - ay_local - ScaleForWindow(hAplicarPanel,10);
                    MoveWindow(hAplicarOutput, axlbl, ay_local, aoutW, aoutH, TRUE);
                }
            }
        }
        break;
    }
    case WM_MEASUREITEM:
        // sin owner-draw salvo tabs en oscuro
        break;
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        switch (id) {
        case 301: // Español
            g_lang = LANG_ES;
            TranslateUI(hwndTab, hCrearPanel, hAplicarPanel,
                hCrearLblImg, hCrearLblMod, hCrearLblPPF, hCrearLblDIZ, hCrearLblDesc,
                hCrearChkUndo, hCrearChkValid, hCrearLblTipo, hCrearComboTipo, hCrearBtnCrear, hCrearBtnShow, hCrearBtnAdd, hCrearBtnClear, NULL,
                hAplicarLblImg, hAplicarLblPPF, hAplicarChkRevert, hAplicarBtnApply, hAplicarBtnClear, hAplicarLblSalida,
                GetMenu(hwnd), GetSubMenu(GetMenu(hwnd), 0), GetSubMenu(GetMenu(hwnd), 1), GetSubMenu(GetMenu(hwnd), 2));
            ForceLayoutRefresh();
            if (g_hwndMain) {
                HMENU hMenuBar = GetMenu(g_hwndMain);
                if (hMenuBar) {
                    HMENU hMenuIdioma = GetSubMenu(hMenuBar, 0);
                    if (hMenuIdioma) {
                        CheckMenuItem(hMenuIdioma, 301, MF_BYCOMMAND | MF_CHECKED);
                        CheckMenuItem(hMenuIdioma, 302, MF_BYCOMMAND | MF_UNCHECKED);
                    }
                    UpdateThemeMenuChecks(hMenuBar, g_isDark);
                }
                DrawMenuBar(g_hwndMain);
            }
            if (hCrearComboTipo) {
                SendMessageW(hCrearComboTipo, CB_RESETCONTENT, 0, 0);
                SendMessageW(hCrearComboTipo, CB_ADDSTRING, 0, (LPARAM)L"BIN");
                SendMessageW(hCrearComboTipo, CB_ADDSTRING, 0, (LPARAM)L"GI");
                SendMessageW(hCrearComboTipo, CB_ADDSTRING, 0, (LPARAM)L"ISO");
                SendMessageW(hCrearComboTipo, CB_SETCURSEL, 0, 0); // Seleccionar BIN por defecto
            }
            if (hwndTab) {
                NMHDR nmhdr = {0};
                nmhdr.hwndFrom = hwndTab;
                nmhdr.idFrom = GetDlgCtrlID(hwndTab);
                nmhdr.code = TCN_SELCHANGE;
                SendMessageW(hwnd, WM_NOTIFY, (WPARAM)nmhdr.idFrom, (LPARAM)&nmhdr);
            }
            break;
        case 302: // English
            g_lang = LANG_EN;
            TranslateUI(hwndTab, hCrearPanel, hAplicarPanel,
                hCrearLblImg, hCrearLblMod, hCrearLblPPF, hCrearLblDIZ, hCrearLblDesc,
                hCrearChkUndo, hCrearChkValid, hCrearLblTipo, hCrearComboTipo, hCrearBtnCrear, hCrearBtnShow, hCrearBtnAdd, hCrearBtnClear, NULL,
                hAplicarLblImg, hAplicarLblPPF, hAplicarChkRevert, hAplicarBtnApply, hAplicarBtnClear, hAplicarLblSalida,
                GetMenu(hwnd), GetSubMenu(GetMenu(hwnd), 0), GetSubMenu(GetMenu(hwnd), 1), GetSubMenu(GetMenu(hwnd), 2));
            ForceLayoutRefresh();
            // Refrescar menú de idioma
            if (g_hwndMain) {
                HMENU hMenuBar = GetMenu(g_hwndMain);
                if (hMenuBar) {
                    HMENU hMenuIdioma = GetSubMenu(hMenuBar, 0);
                    if (hMenuIdioma) {
                        CheckMenuItem(hMenuIdioma, 301, MF_BYCOMMAND | MF_UNCHECKED);
                        CheckMenuItem(hMenuIdioma, 302, MF_BYCOMMAND | MF_CHECKED);
                    }
                    UpdateThemeMenuChecks(hMenuBar, g_isDark);
                }
                DrawMenuBar(g_hwndMain);
            }
            // Combo tipo
            if (hCrearComboTipo) {
                SendMessageW(hCrearComboTipo, CB_RESETCONTENT, 0, 0);
                SendMessageW(hCrearComboTipo, CB_ADDSTRING, 0, (LPARAM)L"BIN");
                SendMessageW(hCrearComboTipo, CB_ADDSTRING, 0, (LPARAM)L"GI");
                SendMessageW(hCrearComboTipo, CB_ADDSTRING, 0, (LPARAM)L"ISO");
                SendMessageW(hCrearComboTipo, CB_SETCURSEL, 0, 0); // Seleccionar BIN por defecto
            }
            // Forzar refresco de la pestaña activa reenviando TCN_SELCHANGE
            if (hwndTab) {
                NMHDR nmhdr = {0};
                nmhdr.hwndFrom = hwndTab;
                nmhdr.idFrom = GetDlgCtrlID(hwndTab);
                nmhdr.code = TCN_SELCHANGE;
                SendMessageW(hwnd, WM_NOTIFY, (WPARAM)nmhdr.idFrom, (LPARAM)&nmhdr);
            }
            break;
        case 203: { // Tema oscuro
            g_themePref = 1;
            ApplyCurrentTheme(true, hwnd, hwndTab, hCrearPanel, hAplicarPanel, hCrearChkUndo, hCrearChkValid, hAplicarChkRevert, GetMenu(hwnd));
            UpdateButtonThemes(g_isDark, botones, 12);
            UpdateControlThemes(g_isDark, themedCtrls, 13);
            if (hCrearComboTipo) InvalidateRect(hCrearComboTipo, NULL, TRUE);
            break;
        }
        case 204: { // Tema claro
            g_themePref = 0;
            ApplyCurrentTheme(false, hwnd, hwndTab, hCrearPanel, hAplicarPanel, hCrearChkUndo, hCrearChkValid, hAplicarChkRevert, GetMenu(hwnd));
            UpdateButtonThemes(g_isDark, botones, 12);
            UpdateControlThemes(g_isDark, themedCtrls, 13);
            if (hCrearComboTipo) InvalidateRect(hCrearComboTipo, NULL, TRUE);
            break;
        }
        case 206: // About
            ShowAboutDialog(hwnd);
            break;
        case 205: // Help
            ShowHelpDialog(hwnd);
            break;
        case 207: // Show help (console help shown in GUI)
            MessageBoxW(hwnd, tw("console_help"), T(L"menu_help"), MB_OK | MB_ICONINFORMATION);
            break;
        // Crear Parche browse buttons
        case 111: // imagen original
        case 112: // imagen modificada
        case 113: // ppf destino
        case 114: // diz
        {
            OPENFILENAMEW ofn;
            wchar_t filename[MAX_PATH] = {0};
            wchar_t initialDir[MAX_PATH] = {0};
            ZeroMemory(&ofn, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFile = filename;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
            DWORD flags = (id == 113) ? 0 : OFN_FILEMUSTEXIST;
            HWND hTarget = (id == 113) ? hCrearEditPPF : (id == 111) ? hCrearEditImg : (id == 112) ? hCrearEditMod : (id == 114) ? hCrearEditDIZ : NULL;
            // unified dialog: set filter according to control id
            {
                static wchar_t filter[256];
                memset(filter, 0, sizeof(filter));
                if (id == 113) {
                    // PPF save
                    wcscpy(filter, tw("filter_ppf"));
                    size_t pos = wcslen(filter) + 1;
                    wcscpy(&filter[pos], L"*.ppf");
                    pos += wcslen(&filter[pos]) + 1;
                    wcscpy(&filter[pos], tw("filter_all"));
                    pos += wcslen(&filter[pos]) + 1;
                    wcscpy(&filter[pos], L"*.*");
                    size_t finalPos = pos + wcslen(&filter[pos]) + 1;
                    if (finalPos + 1 < sizeof(filter)/sizeof(wchar_t)) { filter[finalPos] = L'\0'; filter[finalPos+1] = L'\0'; }
                } else if (id == 111 || id == 112) {
                    // image files
                    wcscpy(filter, tw("filter_images"));
                    size_t pos = wcslen(filter) + 1;
                    wcscpy(&filter[pos], L"*.bin;*.gi;*.iso");
                    pos += wcslen(&filter[pos]) + 1;
                    wcscpy(&filter[pos], tw("filter_all"));
                    pos += wcslen(&filter[pos]) + 1;
                    wcscpy(&filter[pos], L"*.*");
                    size_t finalPos = pos + wcslen(&filter[pos]) + 1;
                    if (finalPos + 1 < sizeof(filter)/sizeof(wchar_t)) { filter[finalPos] = L'\0'; filter[finalPos+1] = L'\0'; }
                } else if (id == 114) {
                    // diz file
                    wcscpy(filter, tw("filter_diz"));
                    size_t pos = wcslen(filter) + 1;
                    wcscpy(&filter[pos], L"*.diz");
                    pos += wcslen(&filter[pos]) + 1;
                    wcscpy(&filter[pos], tw("filter_all"));
                    pos += wcslen(&filter[pos]) + 1;
                    wcscpy(&filter[pos], L"*.*");
                    size_t finalPos = pos + wcslen(&filter[pos]) + 1;
                    if (finalPos + 1 < sizeof(filter)/sizeof(wchar_t)) { filter[finalPos] = L'\0'; filter[finalPos+1] = L'\0'; }
                } else {
                    // fallback all files
                    wcscpy(filter, tw("filter_all"));
                    size_t pos = wcslen(filter) + 1;
                    wcscpy(&filter[pos], L"*.*");
                    size_t finalPos = pos + wcslen(&filter[pos]) + 1;
                    if (finalPos + 1 < sizeof(filter)/sizeof(wchar_t)) { filter[finalPos] = L'\0'; filter[finalPos+1] = L'\0'; }
                }
                ofn.lpstrFilter = filter;

                // initial dir from target if present
                if (hTarget && GetWindowTextW(hTarget, initialDir, MAX_PATH) && wcslen(initialDir) > 0) {
                    GetParentFolder(initialDir, initialDir, MAX_PATH);
                    ofn.lpstrInitialDir = initialDir;
                } else {
                    // Use executable dir
                    static wchar_t exeDir[MAX_PATH] = {0};
                    GetExecutableDirectory(exeDir, MAX_PATH);
                    ofn.lpstrInitialDir = exeDir;
                }

                // Use modern COM dialog
                {
                    HRESULT hrDlg = S_OK;
                    if (ShowSaveFileDialog_COM(hwnd, filename, MAX_PATH, ofn.lpstrInitialDir, ofn.lpstrFilter, &hrDlg, flags)) {
                        if (hTarget) SetWindowTextW(hTarget, filename);
                        // If selecting original image in 'Crear', auto-select ISO if filename ends with .iso
                        if (id == 111) MaybeSetImageTypeFromPath(hCrearComboTipo, hCrearChkValid, filename);
                        // If user selected the original image in 'Crear', auto-fill PPF filename if empty
                        if (id == 111 && hCrearEditPPF) {
                            wchar_t curppf[MAX_PATH] = {0};
                            GetWindowTextW(hCrearEditPPF, curppf, MAX_PATH);
                            if (wcslen(curppf) == 0) {
                                // build ppf path by replacing extension with .ppf
                                wchar_t ppfpath[MAX_PATH];
                                wcscpy(ppfpath, filename);
                                wchar_t *dot = wcsrchr(ppfpath, L'.');
                                if (dot) *dot = L'\0';
                                wcscat(ppfpath, L".ppf");
                                SetWindowTextW(hCrearEditPPF, ppfpath);
                            }
                        }
                    }
                }
            }
            break;
        }
        case 122: // Crear: Validación checkbox clicked (user override)
            if (HIWORD(wParam) == BN_CLICKED) {
                g_crearValidUserSet = 1;
            }
            break;
        case 123: // Crear: Tipo combo selection changed
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                int sel = (int)SendMessageW(hCrearComboTipo, CB_GETCURSEL, 0, 0);
                if (!g_crearValidUserSet && hCrearChkValid) {
                    if (sel == 2) SendMessageW(hCrearChkValid, BM_SETCHECK, BST_UNCHECKED, 0);
                    else SendMessageW(hCrearChkValid, BM_SETCHECK, BST_CHECKED, 0);
                }
            }
            break;
        case 131: // Crear Parche (run MakePPF)
        {
            PROC_THREAD_PARAM *p = (PROC_THREAD_PARAM*)malloc(sizeof(PROC_THREAD_PARAM));
            p->hEdit = hCrearOutput;
            p->partial_len = 0; p->partial[0] = 0;
            // build command line from controls
            BuildCreateCmdLine(p->cmdline, sizeof(p->cmdline)/sizeof(wchar_t), hCrearEditImg, hCrearEditMod, hCrearEditPPF, hCrearEditDIZ, hCrearEditDesc, hCrearChkUndo, hCrearChkValid, hCrearComboTipo);

            // GUI-side validation: ensure essential fields are selected and show friendly localized messages when missing
            {
                wchar_t origname[MAX_PATH] = {0}, modname[MAX_PATH] = {0}, ppfname[MAX_PATH] = {0};
                if (hCrearEditImg) GetWindowTextW(hCrearEditImg, origname, MAX_PATH);
                if (hCrearEditMod) GetWindowTextW(hCrearEditMod, modname, MAX_PATH);
                if (hCrearEditPPF) GetWindowTextW(hCrearEditPPF, ppfname, MAX_PATH);
                if (wcslen(origname) == 0 || wcslen(modname) == 0 || wcslen(ppfname) == 0) {
                    if (wcslen(origname) == 0) AppendTextToEdit(hCrearOutput, tw("select_create_origname"));
                    if (wcslen(modname) == 0) AppendTextToEdit(hCrearOutput, tw("select_create_modname"));
                    if (wcslen(ppfname) == 0) AppendTextToEdit(hCrearOutput, tw("select_create_ppfname"));
                    free(p);
                    break;
                }
            }

            // save current settings
            SaveSettings(hCrearEditImg, hCrearEditMod, hCrearEditPPF, hCrearEditDIZ, hCrearEditDesc, hCrearChkUndo, hCrearChkValid, hCrearComboTipo,
                         hAplicarEditImg, hAplicarEditPPF, hAplicarChkRevert);
            HANDLE hThread = CreateThread(NULL, 0, ProcessCaptureThread, p, 0, NULL);
            if (hThread) CloseHandle(hThread); /* CRITICAL: Close thread handle to prevent leak */
            break;
        }
        case 132: // Ver Info Parche (show)
        {
            PROC_THREAD_PARAM *p = (PROC_THREAD_PARAM*)malloc(sizeof(PROC_THREAD_PARAM));
            p->hEdit = hCrearOutput;
            p->partial_len = 0; p->partial[0] = 0;
            p->cmdline[0] = 0;
            wcscpy(p->cmdline, L"MakePPF");
            wcscat(p->cmdline, L" s");
            wchar_t buf[MAX_PATH];
            if (hCrearEditPPF && GetWindowTextW(hCrearEditPPF, buf, MAX_PATH) && wcslen(buf) > 0) {
                AppendQuotedArg(p->cmdline, sizeof(p->cmdline)/sizeof(wchar_t), buf);
                HANDLE hThread = CreateThread(NULL, 0, ProcessCaptureThread, p, 0, NULL);
                if (hThread) CloseHandle(hThread); /* CRITICAL: Close thread handle to prevent leak */
            } else {
                AppendTextToEdit(hCrearOutput, tw("select_ppf_info"));
                free(p);
            }
            break;
        }
        case 133: // Añadir file_id.diz (f)
        {
            PROC_THREAD_PARAM *p = (PROC_THREAD_PARAM*)malloc(sizeof(PROC_THREAD_PARAM));
            p->hEdit = hCrearOutput;
            p->partial_len = 0; p->partial[0] = 0;
            p->cmdline[0] = 0;
            wcscpy(p->cmdline, L"MakePPF");
            wcscat(p->cmdline, L" f");
            wchar_t ppf[MAX_PATH]; wchar_t fileid[MAX_PATH];
            if (hCrearEditPPF) GetWindowTextW(hCrearEditPPF, ppf, MAX_PATH); else ppf[0]=0;
            if (hCrearEditDIZ) GetWindowTextW(hCrearEditDIZ, fileid, MAX_PATH); else fileid[0]=0;
            if (wcslen(ppf) && wcslen(fileid)) {
                AppendQuotedArg(p->cmdline, sizeof(p->cmdline)/sizeof(wchar_t), ppf);
                AppendQuotedArg(p->cmdline, sizeof(p->cmdline)/sizeof(wchar_t), fileid);
                HANDLE hThread = CreateThread(NULL, 0, ProcessCaptureThread, p, 0, NULL);
                if (hThread) CloseHandle(hThread); /* CRITICAL: Close thread handle to prevent leak */
            } else {
                AppendTextToEdit(hCrearOutput, tw("select_ppf_fileid"));
                free(p);
            }
            break;
        }
        case 134: // Limpiar salida crear
            SetWindowTextW(hCrearOutput, L"");
            /* Reset progress bar to 0 and keep visible but non-interactive */
            CrearProgress_ResetToZero();
            break;

        // Aplicar Parche browse
        case 211:
        case 212:
        {
            OPENFILENAMEW ofn;
            wchar_t filename[MAX_PATH] = {0};
            wchar_t initialDir[MAX_PATH] = {0};
            ZeroMemory(&ofn, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFile = filename;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
            HWND hTarget = (id == 211) ? hAplicarEditImg : hAplicarEditPPF;
            // Set filter: image -> files, ppf -> PPF
            {
                static wchar_t filter[256];
                const wchar_t *lbl_files = tw("filter_images");
                const wchar_t *lbl_all = tw("filter_all");
                const wchar_t *lbl_ppf = tw("filter_ppf");
                if (id == 211) {
                    memset(filter, 0, sizeof(filter));
                    wcscpy(filter, lbl_files);
                    size_t pos = wcslen(filter) + 1;
                    wcscpy(&filter[pos], L"*.bin;*.gi;*.iso");
                    pos += wcslen(&filter[pos]) + 1;
                    wcscpy(&filter[pos], lbl_all);
                    pos += wcslen(&filter[pos]) + 1;
                    wcscpy(&filter[pos], L"*.*");
                    size_t finalPos = pos + wcslen(&filter[pos]) + 1;
                    if (finalPos + 1 < sizeof(filter)/sizeof(wchar_t)) { filter[finalPos] = L'\0'; filter[finalPos+1] = L'\0'; }
                    ofn.lpstrFilter = filter;
                } else {
                    memset(filter, 0, sizeof(filter));
                    wcscpy(filter, lbl_ppf);
                    size_t pos = wcslen(filter) + 1;
                    wcscpy(&filter[pos], L"*.ppf");
                    pos += wcslen(&filter[pos]) + 1;
                    wcscpy(&filter[pos], lbl_all);
                    pos += wcslen(&filter[pos]) + 1;
                    wcscpy(&filter[pos], L"*.*");
                    size_t finalPos = pos + wcslen(&filter[pos]) + 1;
                    if (finalPos + 1 < sizeof(filter)/sizeof(wchar_t)) { filter[finalPos] = L'\0'; filter[finalPos+1] = L'\0'; }
                    ofn.lpstrFilter = filter;
                }
            }
            if (hTarget && GetWindowTextW(hTarget, initialDir, MAX_PATH) && wcslen(initialDir) > 0) {
                GetParentFolder(initialDir, initialDir, MAX_PATH);
                ofn.lpstrInitialDir = initialDir;
            } else {
                // Use executable dir
                static wchar_t exeDir3[MAX_PATH] = {0};
                GetExecutableDirectory(exeDir3, MAX_PATH);
                ofn.lpstrInitialDir = exeDir3;
            }
            // Use modern COM dialog
            HRESULT hrDlg = S_OK;
            BOOL ok = ShowSaveFileDialog_COM(hwnd, filename, MAX_PATH, ofn.lpstrInitialDir, ofn.lpstrFilter, &hrDlg, OFN_FILEMUSTEXIST);
            if (ok) {
                if (hTarget) {
                    SetWindowTextW(hTarget, filename);
                }
                // If user selected a PPF in Aplicar tab, automatically show its info (including embedded file_id)
                if (id == 212) {
                    PROC_THREAD_PARAM *p = (PROC_THREAD_PARAM*)malloc(sizeof(PROC_THREAD_PARAM));
                    if (p) {
                        p->hEdit = hAplicarOutput;
                        p->cmdline[0] = 0;
                        p->partial_len = 0; p->partial[0] = 0;
                        wcscpy(p->cmdline, L"MakePPF");
                        wcscat(p->cmdline, L" s");
                        AppendQuotedArg(p->cmdline, sizeof(p->cmdline)/sizeof(wchar_t), filename);
                        // Don't set progress callback for info display to avoid showing progress bar
                        HANDLE hThread = CreateThread(NULL, 0, ProcessCaptureThread, p, 0, NULL);
                        if (hThread) CloseHandle(hThread); /* CRITICAL: Close thread handle to prevent leak */
                        /* Ensure browse buttons are enabled after starting the worker thread (workaround for intermittent disable bug) */
                        if (g_hwndMain) PostMessageW(g_hwndMain, WM_ENABLE_BROWSE, (WPARAM)1, 0);
                        else PostMessageW(GetForegroundWindow(), WM_ENABLE_BROWSE, (WPARAM)1, 0);
                    }
                }
            }
            break;
        }
        case 231: // Aplicar Parche
        {
            PROC_THREAD_PARAM *p = (PROC_THREAD_PARAM*)malloc(sizeof(PROC_THREAD_PARAM));
            p->hEdit = hAplicarOutput;
            if (p) { p->partial_len = 0; p->partial[0] = 0; }
            BuildApplyCmdLine(p->cmdline, sizeof(p->cmdline)/sizeof(wchar_t), hAplicarEditImg, hAplicarEditPPF, hAplicarChkRevert);

            // GUI-side validation: ensure bin and ppf are selected
            {
                wchar_t img[MAX_PATH] = {0}, ppf[MAX_PATH] = {0};
                if (hAplicarEditImg) GetWindowTextW(hAplicarEditImg, img, MAX_PATH);
                if (hAplicarEditPPF) GetWindowTextW(hAplicarEditPPF, ppf, MAX_PATH);
                if (wcslen(img) == 0 || wcslen(ppf) == 0) {
                    if (wcslen(img) == 0) AppendTextToEdit(hAplicarOutput, tw("select_apply_bin"));
                    if (wcslen(ppf) == 0) AppendTextToEdit(hAplicarOutput, tw("select_apply_ppf"));
                    free(p);
                    break;
                }
            }

            // save current settings
            SaveSettings(hCrearEditImg, hCrearEditMod, hCrearEditPPF, hCrearEditDIZ, hCrearEditDesc, hCrearChkUndo, hCrearChkValid, hCrearComboTipo,
                         hAplicarEditImg, hAplicarEditPPF, hAplicarChkRevert);
                HANDLE hThread = CreateThread(NULL, 0, ProcessCaptureThread, p, 0, NULL);
                if (hThread) CloseHandle(hThread); /* CRITICAL: Close thread handle to prevent leak */
            break;
        }
        case 232: // Limpiar salida aplicar
            SetWindowTextW(hAplicarOutput, L"");
            /* Reset create-tab progress to zero but keep visible and non-interactive */
            CrearProgress_ResetToZero();
            /* Reset apply-tab progress to zero but keep visible and non-interactive */
            AplicarProgress_ResetToZero();
            break;
        }
        break;
    }
    case WM_NOTIFY: {
        NMHDR *pnmhdr = (NMHDR*)lParam;
        if (pnmhdr->hwndFrom == hwndTab && pnmhdr->code == NM_CUSTOMDRAW && g_isDark) {
            LPNMCUSTOMDRAW pcd = (LPNMCUSTOMDRAW)lParam;
            switch (pcd->dwDrawStage) {
            case CDDS_PREPAINT:
                // Pintar TODA el área del control de oscuro
                {
                    RECT rc; GetClientRect(hwndTab, &rc);
                    // Pintar todo el rect expandido
                    rc.left -= 10; rc.top -= 10; rc.right += 10; rc.bottom += 10;
                    FillRect(pcd->hdc, &rc, g_brBg);
                    GetClientRect(hwndTab, &rc);
                    FillRect(pcd->hdc, &rc, g_brBg);
                }
                return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT | CDRF_SKIPDEFAULT;
            case CDDS_ITEMPREPAINT: {
                int idx = (int)pcd->dwItemSpec;
                RECT rcTab; TabCtrl_GetItemRect(hwndTab, idx, &rcTab);
                COLORREF clrTab = (idx == TabCtrl_GetCurSel(hwndTab)) ? RGB(45,45,45) : g_clrBg;
                HBRUSH hFill = CreateSolidBrush(clrTab);
                FillRect(pcd->hdc, &rcTab, hFill);
                DeleteObject(hFill);

                wchar_t text[256] = {0};
                TCITEMW tie = {0}; tie.mask = TCIF_TEXT; tie.pszText = text; tie.cchTextMax = 256;
                TabCtrl_GetItem(hwndTab, idx, &tie);
                RECT rcTxt = rcTab; InflateRect(&rcTxt, -6, -2); rcTxt.bottom += 1;
                SetBkMode(pcd->hdc, TRANSPARENT);
                SetTextColor(pcd->hdc, g_clrText);
                DrawTextW(pcd->hdc, text, -1, &rcTxt, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                return CDRF_SKIPDEFAULT;
            }
            case CDDS_POSTPAINT: {
                // Pintar TODO el área del control de nuevo
                RECT rc; GetClientRect(hwndTab, &rc);
                RECT rcDisplay = rc;
                TabCtrl_AdjustRect(hwndTab, FALSE, &rcDisplay);
                // Pintar desde donde terminan las pestañas hasta el final
                RECT rcBottom = rc;
                rcBottom.top = rcDisplay.top - 10;
                FillRect(pcd->hdc, &rcBottom, g_brBg);
                return CDRF_SKIPDEFAULT;
            }
            }
        }
        if (pnmhdr->hwndFrom == hwndTab && pnmhdr->code == TCN_SELCHANGE) {
            int nTabIndex = TabCtrl_GetCurSel(hwndTab);
            if (nTabIndex == 0) {
                // Mostrar panel Crear, ocultar panel Aplicar
                ShowWindow(hCrearPanel, SW_SHOW);
                ShowWindow(hAplicarPanel, SW_HIDE);
            } else if (nTabIndex == 1) {
                // Mostrar panel Aplicar, ocultar panel Crear
                ShowWindow(hCrearPanel, SW_HIDE);
                ShowWindow(hAplicarPanel, SW_SHOW);
            }
        }
        break;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT*)lParam;
        if (dis->CtlType == ODT_TAB && dis->hwndItem == hwndTab && g_isDark) {
            int idx = (int)dis->itemID;
            RECT rcTab = dis->rcItem;
            InflateRect(&rcTab, -1, -1); // borde fino
            COLORREF clrTab = (dis->itemState & ODS_SELECTED) ? RGB(45,45,45) : g_clrBg;
            HBRUSH hFill = CreateSolidBrush(clrTab);
            FillRect(dis->hDC, &rcTab, hFill);
            DeleteObject(hFill);

            wchar_t text[256] = {0};
            TCITEMW tie = {0}; tie.mask = TCIF_TEXT; tie.pszText = text; tie.cchTextMax = 256;
            TabCtrl_GetItem(hwndTab, idx, &tie);
            RECT rcTxt = rcTab; InflateRect(&rcTxt, -5, 0); rcTxt.bottom += 4; // espacio extra para no recortar
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, g_clrText);
            DrawTextW(dis->hDC, text, -1, &rcTxt, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            return TRUE; // handled
        }
        break;
    }
    case WM_DESTROY:
        // Destroy DPI-loaded icons
        if (g_hIconBig) { DestroyIcon(g_hIconBig); g_hIconBig = NULL; }
        if (g_hIconSmall) { DestroyIcon(g_hIconSmall); g_hIconSmall = NULL; }
        // persist settings on exit
        SaveSettings(hCrearEditImg, hCrearEditMod, hCrearEditPPF, hCrearEditDIZ, hCrearEditDesc, hCrearChkUndo, hCrearChkValid, hCrearComboTipo,
                     hAplicarEditImg, hAplicarEditPPF, hAplicarChkRevert);
        // Delete created fonts
        if (hFont) { DeleteObject(hFont); hFont = NULL; }
        if (hMonoFont) { DeleteObject(hMonoFont); hMonoFont = NULL; }
        PostQuitMessage(0);
        break;
    case WM_DPICHANGED: {
        // lParam contains suggested new window rect
        RECT *prc = (RECT*)lParam;
        if (prc) {
            SetWindowPos(hwnd, NULL, prc->left, prc->top, prc->right - prc->left, prc->bottom - prc->top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        int newDpi = GetWindowDPI(hwnd);
        // reload DPI-appropriate icons
        LoadAndSetIconsForDPI(hwnd, newDpi);
        // Recreate fonts at new DPI
        if (hFont) { DeleteObject(hFont); hFont = NULL; }
        if (hMonoFont) { DeleteObject(hMonoFont); hMonoFont = NULL; }
        LOGFONTW lf = {0}; lf.lfHeight = -MulDiv(10, newDpi, 72); wcscpy(lf.lfFaceName, L"Segoe UI variable"); hFont = CreateFontIndirectW(&lf);
        LOGFONTW lfm = {0}; lfm.lfHeight = -MulDiv(10, newDpi, 72); wcscpy(lfm.lfFaceName, L"Consolas"); hMonoFont = CreateFontIndirectW(&lfm);
        if (!hMonoFont) { wcscpy(lfm.lfFaceName, L"Courier New"); hMonoFont = CreateFontIndirectW(&lfm); }
        // Apply new fonts/icons to controls
        if (hwndTab) SendMessageW(hwndTab, WM_SETFONT, (WPARAM)hFont, TRUE);
        if (hCrearPanel) SendMessageW(hCrearPanel, WM_SETFONT, (WPARAM)hFont, TRUE);
        if (hAplicarPanel) SendMessageW(hAplicarPanel, WM_SETFONT, (WPARAM)hFont, TRUE);
        // set font on direct children
        if (hCrearPanel) EnumChildWindows(hCrearPanel, SetFontEnumProc, (LPARAM)hFont);
        if (hAplicarPanel) EnumChildWindows(hAplicarPanel, SetFontEnumProc, (LPARAM)hFont);
        // trigger layout recalculation
        SendMessageW(hwnd, WM_SIZE, 0, 0);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// Helper: set font on all immediate child controls of a parent
static BOOL CALLBACK SetFontEnumProc(HWND hwndChild, LPARAM lParam) {
    HFONT hf = (HFONT)lParam;
    if (hf) SendMessageW(hwndChild, WM_SETFONT, (WPARAM)hf, TRUE);
    return TRUE;
}

// Subclass procedure for Crear panel: forward WM_COMMAND and paint with current theme
LRESULT CALLBACK CrearPanelProc(HWND hwndPanel, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_COMMAND:
        if (g_hwndMain) SendMessageW(g_hwndMain, WM_COMMAND, wParam, lParam);
        return 0;
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc; GetClientRect(hwndPanel, &rc);
        FillRect(hdc, &rc, g_brBg);
        return 1;
    }
    case WM_CTLCOLORSTATIC: {
        HWND hCtrl = (HWND)lParam;
        HDC hdc = (HDC)wParam;
        if (hCtrl == g_hCrearOutput || hCtrl == g_hAplicarOutput) {
            SetTextColor(hdc, g_clrEditText);
            SetBkColor(hdc, g_clrEditBg);
            return (LRESULT)g_brEditBg;
        }
        SetTextColor(hdc, g_clrText);
        SetBkColor(hdc, g_clrBg);
        return (LRESULT)g_brBg;
    }
    case WM_CTLCOLORBTN: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, g_clrText);
        SetBkColor(hdc, g_clrBg);
        return (LRESULT)g_brBg;
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, g_clrEditText);
        SetBkColor(hdc, g_clrEditBg);
        return (LRESULT)g_brEditBg;
    }
    case WM_CTLCOLORLISTBOX: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, g_clrEditText);
        SetBkColor(hdc, g_clrEditBg);
        return (LRESULT)g_brEditBg;
    }
    default:
        break;
    }
    if (oldCrearPanelProc) return CallWindowProcW(oldCrearPanelProc, hwndPanel, msg, wParam, lParam);
    return DefWindowProcW(hwndPanel, msg, wParam, lParam);
}

// Subclass procedure for Aplicar panel
LRESULT CALLBACK AplicarPanelProc(HWND hwndPanel, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_COMMAND:
        if (g_hwndMain) SendMessageW(g_hwndMain, WM_COMMAND, wParam, lParam);
        return 0;
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc; GetClientRect(hwndPanel, &rc);
        FillRect(hdc, &rc, g_brBg);
        return 1;
    }
    case WM_CTLCOLORSTATIC: {
        HWND hCtrl = (HWND)lParam;
        HDC hdc = (HDC)wParam;
        if (hCtrl == g_hCrearOutput || hCtrl == g_hAplicarOutput) {
            SetTextColor(hdc, g_clrEditText);
            SetBkColor(hdc, g_clrEditBg);
            return (LRESULT)g_brEditBg;
        }
        SetTextColor(hdc, g_clrText);
        SetBkColor(hdc, g_clrBg);
        return (LRESULT)g_brBg;
    }
    case WM_CTLCOLORBTN: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, g_clrText);
        SetBkColor(hdc, g_clrBg);
        return (LRESULT)g_brBg;
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, g_clrEditText);
        SetBkColor(hdc, g_clrEditBg);
        return (LRESULT)g_brEditBg;
    }
    case WM_CTLCOLORLISTBOX: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, g_clrEditText);
        SetBkColor(hdc, g_clrEditBg);
        return (LRESULT)g_brEditBg;
    }
    default:
        break;
    }
    if (oldAplicarPanelProc) return CallWindowProcW(oldAplicarPanelProc, hwndPanel, msg, wParam, lParam);
    return DefWindowProcW(hwndPanel, msg, wParam, lParam);
}

// Subclass procedure for Tab control: paint background with current theme
LRESULT CALLBACK TabProc(HWND hwndTab, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT:
        {
            PAINTSTRUCT ps; HDC hdc = BeginPaint(hwndTab, &ps);
            RECT rc; GetClientRect(hwndTab, &rc);
            FillRect(hdc, &rc, g_brBg);

            int nTabs = TabCtrl_GetItemCount(hwndTab);
            int nSelected = TabCtrl_GetCurSel(hwndTab);
            if (g_tabLastSelected == -1) g_tabLastSelected = nSelected;
            for (int i = 0; i < nTabs; i++) {
                RECT rcTab; TabCtrl_GetItemRect(hwndTab, i, &rcTab);
                bool isSelected = (i == nSelected);

                COLORREF fill;
                COLORREF textCol;
                // Animation interpolation (fade)
                if (g_tabAnimFrom != -1 && (i == g_tabAnimFrom || i == g_tabAnimTo)) {
                    if (i == g_tabAnimTo) {
                        fill = LerpColor(g_clrTabNorm, g_clrTabSel, g_tabAnimStep, TAB_ANIM_STEPS);
                        textCol = LerpColor(g_clrTabTextNorm, g_clrTabTextSel, g_tabAnimStep, TAB_ANIM_STEPS);
                    } else {
                        fill = LerpColor(g_clrTabSel, g_clrTabNorm, g_tabAnimStep, TAB_ANIM_STEPS);
                        textCol = LerpColor(g_clrTabTextSel, g_clrTabTextNorm, g_tabAnimStep, TAB_ANIM_STEPS);
                    }
                } else {
                    fill = isSelected ? g_clrTabSel : g_clrTabNorm;
                    textCol = isSelected ? g_clrTabTextSel : g_clrTabTextNorm;
                    if (!isSelected && i == g_tabHover) fill = LerpColor(fill, g_clrTabSel, 1, 6);
                }

                HBRUSH hBr = CreateSolidBrush(fill);
                FillRect(hdc, &rcTab, hBr);
                DeleteObject(hBr);

                // Top edge line: use theme-aware colors to avoid visible black line in light mode
                if (isSelected || (g_tabAnimTo == i && g_tabAnimStep > 0)) {
                    COLORREF topColor = g_isDark ? RGB(150,150,150) : RGB(120,120,120);
                    HPEN hTop = CreatePen(PS_SOLID, 2, topColor);
                    HPEN hOld = (HPEN)SelectObject(hdc, hTop);
                    MoveToEx(hdc, rcTab.left, rcTab.top, NULL);
                    LineTo(hdc, rcTab.right, rcTab.top);
                    SelectObject(hdc, hOld);
                    DeleteObject(hTop);
                } else {
                    COLORREF edgeColor = g_isDark ? RGB(55,55,55) : RGB(200,200,200);
                    HPEN hEdge = CreatePen(PS_SOLID, 1, edgeColor);
                    HPEN hOld = (HPEN)SelectObject(hdc, hEdge);
                    MoveToEx(hdc, rcTab.left, rcTab.top, NULL);
                    LineTo(hdc, rcTab.right, rcTab.top);
                    SelectObject(hdc, hOld);
                    DeleteObject(hEdge);
                }

                wchar_t text[256] = {0};
                TCITEMW tie = {0}; tie.mask = TCIF_TEXT; tie.pszText = text; tie.cchTextMax = 256;
                TabCtrl_GetItem(hwndTab, i, &tie);
                RECT rcTxt = rcTab; InflateRect(&rcTxt, -6, -2); rcTxt.bottom += 1;

                HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, textCol);
                DrawTextW(hdc, text, -1, &rcTxt, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                SelectObject(hdc, hOldFont);
            }

            // content area
            RECT rcDisplay = rc; TabCtrl_AdjustRect(hwndTab, FALSE, &rcDisplay);
            FillRect(hdc, &rcDisplay, g_brBg);
            HPEN hBorder = CreatePen(PS_SOLID, 1, g_clrBorder ? g_clrBorder : RGB(70,70,70));
            HPEN hOld = (HPEN)SelectObject(hdc, hBorder);
            MoveToEx(hdc, rcDisplay.left - 1, rcDisplay.top - 1, NULL);
            LineTo(hdc, rcDisplay.right, rcDisplay.top - 1);
            LineTo(hdc, rcDisplay.right, rcDisplay.bottom);
            LineTo(hdc, rcDisplay.left - 1, rcDisplay.bottom);
            LineTo(hdc, rcDisplay.left - 1, rcDisplay.top - 1);
            SelectObject(hdc, hOld);
            DeleteObject(hBorder);

            EndPaint(hwndTab, &ps);
            return 0; // handled
        }
        break;

    case WM_MOUSEMOVE: {
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        TC_HITTESTINFO ht = {0}; ht.pt = pt;
        int idx = TabCtrl_HitTest(hwndTab, &ht);
        if (idx != g_tabHover) {
            int old = g_tabHover; g_tabHover = (idx >= 0) ? idx : -1;
            if (old >= 0) { RECT r; TabCtrl_GetItemRect(hwndTab, old, &r); InvalidateRect(hwndTab, &r, TRUE); }
            if (g_tabHover >= 0) { RECT r; TabCtrl_GetItemRect(hwndTab, g_tabHover, &r); InvalidateRect(hwndTab, &r, TRUE); }
        }
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwndTab, 0 };
        TrackMouseEvent(&tme);
        break;
    }

    case WM_MOUSELEAVE: {
        if (g_tabHover != -1) {
            int old = g_tabHover; g_tabHover = -1; RECT r; TabCtrl_GetItemRect(hwndTab, old, &r); InvalidateRect(hwndTab, &r, TRUE);
        }
        break;
    }

    case WM_LBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        TC_HITTESTINFO ht = {0}; ht.pt = pt;
        int idx = TabCtrl_HitTest(hwndTab, &ht);
        int cur = TabCtrl_GetCurSel(hwndTab);
        if (idx >= 0 && idx != cur) {
            g_tabAnimFrom = cur;
            g_tabAnimTo = idx;
            g_tabAnimStep = 0;
            // set selection ourselves to avoid default immediate repaint that causes double-flash
            TabCtrl_SetCurSel(hwndTab, idx);
            SetTimer(hwndTab, ID_TAB_ANIM_TIMER, TAB_ANIM_INTERVAL, NULL);
            if (g_tabAnimFrom >= 0) { RECT r; TabCtrl_GetItemRect(hwndTab, g_tabAnimFrom, &r); InvalidateRect(hwndTab, &r, TRUE); }
            if (g_tabAnimTo >= 0) { RECT r; TabCtrl_GetItemRect(hwndTab, g_tabAnimTo, &r); InvalidateRect(hwndTab, &r, TRUE); }
            g_tabLastSelected = idx;
        }
        return 0;
    }

    case WM_TIMER: {
        if (wParam == ID_TAB_ANIM_TIMER) {
            if (g_tabAnimStep < TAB_ANIM_STEPS) {
                g_tabAnimStep++;
                if (g_tabAnimFrom >= 0) { RECT r; TabCtrl_GetItemRect(hwndTab, g_tabAnimFrom, &r); InvalidateRect(hwndTab, &r, TRUE); }
                if (g_tabAnimTo >= 0) { RECT r; TabCtrl_GetItemRect(hwndTab, g_tabAnimTo, &r); InvalidateRect(hwndTab, &r, TRUE); }
            }
            if (g_tabAnimStep >= TAB_ANIM_STEPS) { KillTimer(hwndTab, ID_TAB_ANIM_TIMER); g_tabAnimFrom = -1; g_tabAnimTo = -1; g_tabAnimStep = 0; }
            break;
        }
        break;
    }

    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc; GetClientRect(hwndTab, &rc);
        FillRect(hdc, &rc, g_brBg);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, g_clrText);
        SetBkColor(hdc, g_clrBg);
        return (LRESULT)g_brBg;
    }
    default:
        break;
    }
    if (oldTabProc) return CallWindowProcW(oldTabProc, hwndTab, msg, wParam, lParam);
    return DefWindowProcW(hwndTab, msg, wParam, lParam);
}

// Subclass for combo to paint dark button/field
LRESULT CALLBACK ComboProc(HWND hwndCombo, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwndCombo, &ps);
        RECT rc; GetClientRect(hwndCombo, &rc);
        int arrowW = GetSystemMetrics(SM_CXVSCROLL);
        RECT rcBtn = rc; rcBtn.left = rc.right - arrowW;
        RECT rcText = rc; rcText.right = rcBtn.left - 1;
        
        // background principal
        HBRUSH hBg = CreateSolidBrush(g_clrEditBg);
        FillRect(hdc, &rc, hBg);
        DeleteObject(hBg);
        
        // borde sutil Windows 11
        HPEN hBorder = CreatePen(PS_SOLID, 1, g_clrBorder);
        HGDIOBJ oldPen = SelectObject(hdc, hBorder);
        MoveToEx(hdc, rc.left, rc.top, NULL);
        LineTo(hdc, rc.right, rc.top);
        LineTo(hdc, rc.right, rc.bottom);
        LineTo(hdc, rc.left, rc.bottom);
        LineTo(hdc, rc.left, rc.top);
        SelectObject(hdc, oldPen);
        DeleteObject(hBorder);
        
        // separador vertical entre texto y botón
        HPEN hSeparator = CreatePen(PS_SOLID, 1, g_isDark ? RGB(55,55,55) : RGB(215,215,215));
        oldPen = SelectObject(hdc, hSeparator);
        MoveToEx(hdc, rcBtn.left, rc.top, NULL);
        LineTo(hdc, rcBtn.left, rc.bottom);
        SelectObject(hdc, oldPen);
        DeleteObject(hSeparator);
        
        // button background - más sutil
        HBRUSH hBtn = CreateSolidBrush(g_isDark ? RGB(45,45,45) : RGB(240,240,240));
        FillRect(hdc, &rcBtn, hBtn);
        DeleteObject(hBtn);
        
        // arrow - más refinada
        int midX = (rcBtn.left + rcBtn.right) / 2;
        int midY = (rcBtn.top + rcBtn.bottom) / 2;
        HPEN hArrow = CreatePen(PS_SOLID, 1, g_clrText);
        oldPen = SelectObject(hdc, hArrow);
        POINT pts[3] = {{midX - 3, midY - 1}, {midX + 3, midY - 1}, {midX, midY + 2}};
        SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Polygon(hdc, pts, 3);
        SelectObject(hdc, oldPen);
        DeleteObject(hArrow);
        
        // text
        wchar_t text[64] = {0};
        int sel = (int)SendMessageW(hwndCombo, CB_GETCURSEL, 0, 0);
        if (sel >= 0) SendMessageW(hwndCombo, CB_GETLBTEXT, sel, (LPARAM)text);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, g_clrEditText);
        HFONT oldf = (HFONT)SelectObject(hdc, (HFONT)SendMessage(hwndCombo, WM_GETFONT, 0, 0));
        InflateRect(&rcText, -6, -2);
        DrawTextW(hdc, text, -1, &rcText, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        if (oldf) SelectObject(hdc, oldf);
        EndPaint(hwndCombo, &ps);
        return 0;
    }
    default:
        break;
    }
    if (oldComboProc) return CallWindowProcW(oldComboProc, hwndCombo, msg, wParam, lParam);
    return DefWindowProcW(hwndCombo, msg, wParam, lParam);
}

// About dialog window proc (simple, OK centered at bottom)
LRESULT CALLBACK AboutWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            const wchar_t *text = (const wchar_t*)((LPCREATESTRUCTW)lParam)->lpCreateParams;
            RECT rc; GetClientRect(hwnd, &rc);
            /* Create Segoe UI 10pt to match main UI and store it on the window so we can free it at destroy */
            int dpi = GetWindowDPI(hwnd);
            LOGFONTW lf = {0}; lf.lfHeight = -MulDiv(10, dpi, 72); wcscpy(lf.lfFaceName, L"Segoe UI");
            HFONT hf = CreateFontIndirectW(&lf);
            if (hf) SetPropW(hwnd, L"AboutFont", (HANDLE)hf);
            /* static area fits text with margins; keep simple centered layout */
            int margin = ScaleForWindow(hwnd,12);
            int btnH = ScaleForWindow(hwnd,28);
            RECT rcStatic = { margin, margin, rc.right - margin, rc.bottom - margin - (btnH + ScaleForWindow(hwnd,12)) };
            HWND hStatic = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_CENTER, rcStatic.left, rcStatic.top, rcStatic.right - rcStatic.left, rcStatic.bottom - rcStatic.top, hwnd, NULL, GetModuleHandleW(NULL), NULL);
            if (hf) SendMessageW(hStatic, WM_SETFONT, (WPARAM)hf, TRUE);
            // For help dialog, align text to the left
            wchar_t title[256];
            GetWindowTextW(hwnd, title, 256);
            if (wcscmp(title, T(L"help_title")) == 0) {
                SetWindowLongPtrW(hStatic, GWL_STYLE, WS_CHILD | WS_VISIBLE | SS_LEFT);
                InvalidateRect(hStatic, NULL, TRUE);
            }
            const wchar_t *okText = (g_lang == LANG_EN) ? L"OK" : L"Aceptar";
            int okW = ScaleForWindow(hwnd,80);
            HWND hBtn = CreateWindowW(L"BUTTON", okText, BTN_STYLE_DEFAULT, (rc.right - okW)/2, rc.bottom - btnH - ScaleForWindow(hwnd,8), okW, btnH, hwnd, (HMENU)IDOK, GetModuleHandleW(NULL), NULL);
            if (hf) SendMessageW(hBtn, WM_SETFONT, (WPARAM)hf, TRUE);
            SetFocus(hBtn);
            return 0;
        }
        case WM_DESTROY: {
            HFONT phf = (HFONT)RemovePropW(hwnd, L"AboutFont");
            if (phf) DeleteObject(phf);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_CTLCOLORSTATIC:
            SetTextColor((HDC)wParam, g_clrText);
            SetBkColor((HDC)wParam, g_clrBg);
            return (LRESULT)g_brBg;
        case WM_CTLCOLORBTN:
            SetTextColor((HDC)wParam, g_clrText);
            SetBkColor((HDC)wParam, g_clrBg);
            return (LRESULT)g_brBg;
        case WM_ERASEBKGND:
            {
                HDC hdc = (HDC)wParam;
                RECT rc;
                GetClientRect(hwnd, &rc);
                FillRect(hdc, &rc, g_brBg);
                return 1;
            }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Punto de entrada principal Unicode para aplicaciones WinAPI
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance; (void)lpCmdLine; 
    // --- MODO CONSOLA ---
    int argc = 0;
    LPWSTR *argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    HWND hCon = GetConsoleWindow();
    BOOL had_console_at_start = (hCon != NULL);
    BOOL inherited_console = FALSE;
    if (hCon) {
        DWORD pids[8];
        DWORD count = GetConsoleProcessList(pids, (DWORD)(sizeof(pids) / sizeof(pids[0])));
        if (count > 1) inherited_console = TRUE;
    }

    // Single EXE behavior:
    // - If args are present, run CLI (attach to parent console if needed).
    // - If launched from a shell (inherited console), run CLI even with no args (show help).
    // - If launched by double-click (console exists but not inherited), do NOT run CLI; detach and run GUI.
    BOOL attached_console = FALSE;
    if (argc > 1) {
        // Check if stdout is already redirected (e.g., by PowerShell)
        if (_fileno(stdout) < 0) {
            if (!hCon) {
                if (AttachConsole(ATTACH_PARENT_PROCESS)) {
                    hCon = GetConsoleWindow();
                }
            }
            attached_console = (hCon != NULL);
        } else {
            // stdout is already valid, assume we're in a redirected environment
            attached_console = TRUE;
        }
    } else if (inherited_console) {
        attached_console = TRUE;
    } else {
        if (hCon && !inherited_console) {
            FreeConsole();
            hCon = NULL;
        }
    }

    if (attached_console) {
        g_console_attached = 1;
    }

    if (attached_console) {
        // Only redirect stdio if we attached to console; if stdout was already valid, keep it as is
        BOOL did_attach = (argc > 1 && _fileno(stdout) < 0 && hCon != NULL);
        if (did_attach) {
            // Redirigir stdio al mismo console (no abrir una nueva ventana)
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
            freopen("CONIN$", "r", stdin);
            setvbuf(stdout, NULL, _IONBF, 0);
            setvbuf(stderr, NULL, _IONBF, 0);

            // Establecer CP UTF-8 para mostrar correctamente caracteres especiales (ñ, á, etc.)
            SetConsoleOutputCP(CP_UTF8);
            SetConsoleCP(CP_UTF8);
            // Intentar activar secuencias ANSI si es posible
            HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
            DWORD mode = 0;
            if (hOut && GetConsoleMode(hOut, &mode)) {
                SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
            }
        }

        // Establecer la locale a UTF-8 (ayuda con conversiones de banda estrecha)
        setlocale(LC_ALL, ".UTF-8");
        /* Detectar idioma del sistema para modo consola (se puede sobreescribir con INI o variable de entorno) */
        SetLangFromSystem();
        // Si existe settings.ini, preferir la configuración guardada (LangEn)
        {
            wchar_t inipath[MAX_PATH];
            GetSettingsFilePath(inipath, MAX_PATH);
            int langEnIni = GetPrivateProfileIntW(L"Window", L"LangEn", -1, inipath);
            if (langEnIni == 0) g_lang = LANG_ES;
            else if (langEnIni == 1) g_lang = LANG_EN;
        }
        // Comprobar variable de entorno PPFMANAGER_LANG (puede ser 'es' o 'en') para forzar idioma
        {
            wchar_t envLang[16] = {0};
            if (GetEnvironmentVariableW(L"PPFMANAGER_LANG", envLang, sizeof(envLang)/sizeof(wchar_t)) > 0) {
                if (_wcsicmp(envLang, L"es") == 0 || _wcsicmp(envLang, L"es-ES") == 0) g_lang = LANG_ES;
                else if (_wcsicmp(envLang, L"en") == 0 || _wcsicmp(envLang, L"en-US") == 0) g_lang = LANG_EN;
            }
        }

        char **argv = (char **)malloc(argc * sizeof(char*));
        for (int i = 0; i < argc; ++i) {
            /* Use CP_ACP for argv conversion to keep behavior consistent with GUI-mode
               (GUI spawns/executes MakePPF with ANSI/CP_ACP conversion). This avoids
               differing bytes (UTF-8 vs ANSI) for description/file fields that would
               change the produced PPF CRC depending on caller. */
            int len = WideCharToMultiByte(CP_ACP, 0, argvW[i], -1, NULL, 0, NULL, NULL);
            if (len <= 0) len = WideCharToMultiByte(CP_UTF8, 0, argvW[i], -1, NULL, 0, NULL, NULL);
            argv[i] = (char*)malloc(len);
            if (len > 0) {
                if (!WideCharToMultiByte(CP_ACP, 0, argvW[i], -1, argv[i], len, NULL, NULL)) {
                    /* Fallback to UTF-8 if ACP conversion fails */
                    WideCharToMultiByte(CP_UTF8, 0, argvW[i], -1, argv[i], len, NULL, NULL);
                }
            }
        }
        int handled = 0;
        if (argc > 1) {
            // --- MakePPF modo consola ---
            if (argv[1][0] == 'c' || argv[1][0] == 's' || argv[1][0] == 'f') {
                extern void CheckSwitches(int argc, char **argv);
                extern int OpenFilesForCreate(void);
                extern void PPFCreatePatch(void);
                extern void CloseAllFiles(void);
                extern int CheckIfPPF3(void);
                extern void PPFShowPatchInfo(void);
                extern int PPFAddFileId(void);
                extern int CheckIfFileId(void);
                extern int ppf, fileid;
                handled = 1;
                if (argv[1][0] == 'c') {
                    if (argc < 5) {
                        ConsolePutW(tw("usage_c"));
                    } else {
                        /* Initialize MakePPF arguments to the same defaults used by MakePPF_Main to
                           avoid stale/uninitialized state when called in-process. */
                        MakePPF_InitArgs();

                        CheckSwitches(argc, argv);
                        if (OpenFilesForCreate()) {
                            StdoutRedirect redirect; if (RedirectStdout(&redirect)) {
                                PPFCreatePatch();
                                RestoreStdout(&redirect);
                                if (redirect.buffer && redirect.buffer[0]) {
                                    // Process captured buffer lines and translate
                                    char *start = redirect.buffer; char *end = redirect.buffer + redirect.buffer_size;
                                    while (start < end) {
                                        char *nl = (char*)memchr(start, '\n', end - start);
                                        size_t linelen = nl ? (size_t)(nl - start + 1) : (size_t)(end - start);
                                        if (linelen == 0) { start = nl ? nl + 1 : end; continue; }
                                        char linebuf[4096]; if (linelen >= sizeof(linebuf)) linelen = sizeof(linebuf)-1;
                                        memcpy(linebuf, start, linelen); linebuf[linelen]=0;
                                        size_t real_len = linelen; while (real_len > 0 && (linebuf[real_len-1]=='\n' || linebuf[real_len-1]=='\r')) linebuf[--real_len]=0;
                                        int wlen = MultiByteToWideChar(CP_UTF8, 0, linebuf, -1, NULL, 0);
                                        wchar_t *wline = NULL; if (wlen <= 0) { wlen = MultiByteToWideChar(CP_ACP, 0, linebuf, -1, NULL, 0); if (wlen>0) { wline=(wchar_t*)malloc(wlen*sizeof(wchar_t)); MultiByteToWideChar(CP_ACP,0,linebuf,-1,wline,wlen);} } else { wline=(wchar_t*)malloc(wlen*sizeof(wchar_t)); MultiByteToWideChar(CP_UTF8,0,linebuf,-1,wline,wlen);} 
                                        if (wline) { wchar_t *tline = TranslateConsoleLine(wline); free(wline); if (tline) { ConsolePutW(tline); ConsolePutW(L"\r\n"); free(tline);} }
                                        start = nl ? nl + 1 : end;
                                    }
                                }
                                if (redirect.buffer) free(redirect.buffer);
                            } else {
                                // fallback: call directly
                                PPFCreatePatch();
                            }
                            ConsolePutW(tw("patch_created")); ConsolePutW(L"\r\n"); fflush(stdout);
                        } else {
                            ConsolePutW(tw("error_could_not_open_files_create"));
                        }
                        CloseAllFiles();
                    }
                } else if (argv[1][0] == 's') {
                    if (argc < 3) {
                        ConsolePutW(tw("usage_s"));
                    } else {
                        /* Print header exactly as MakePPF does, before any other output. Prefix with a blank line so it starts on its own line in CMD for GUI-mode executables. */
                        ConsolePrintfKeyMB("makeppf_header", __DATE__);
                        fflush(stdout);

                        ppf = _open(argv[2], _O_RDONLY | _O_BINARY);
                        if (ppf == -1) {
                            ConsolePrintfKeyMB("error_cannot_open_file", argv[2]);
                        } else if (!CheckIfPPF3()) {
                            ConsolePrintfKeyMB("error_not_ppf3", argv[2]);
                            _close(ppf);
                        } else {
                            // Capture and translate PPFShowPatchInfo output
                            StdoutRedirect redirect; if (RedirectStdout(&redirect)) {
                                PPFShowPatchInfo();
                                RestoreStdout(&redirect);
                                if (redirect.buffer && redirect.buffer[0]) {
                                    char *start = redirect.buffer; char *end = redirect.buffer + redirect.buffer_size;
                                    while (start < end) {
                                        char *nl = (char*)memchr(start, '\n', end - start);
                                        size_t linelen = nl ? (size_t)(nl - start + 1) : (size_t)(end - start);
                                        if (linelen == 0) { start = nl ? nl + 1 : end; continue; }
                                        char linebuf[4096]; if (linelen >= sizeof(linebuf)) linelen = sizeof(linebuf)-1;
                                        memcpy(linebuf, start, linelen); linebuf[linelen]=0;
                                        size_t real_len = linelen; while (real_len > 0 && (linebuf[real_len-1]=='\n' || linebuf[real_len-1]=='\r')) linebuf[--real_len]=0;
                                        int wlen = MultiByteToWideChar(CP_UTF8, 0, linebuf, -1, NULL, 0);
                                        wchar_t *wline = NULL; if (wlen <= 0) { wlen = MultiByteToWideChar(CP_ACP, 0, linebuf, -1, NULL, 0); if (wlen>0) { wline=(wchar_t*)malloc(wlen*sizeof(wchar_t)); MultiByteToWideChar(CP_ACP,0,linebuf,-1,wline,wlen);} } else { wline=(wchar_t*)malloc(wlen*sizeof(wchar_t)); MultiByteToWideChar(CP_UTF8,0,linebuf,-1,wline,wlen);} 
                                        if (wline) { wchar_t *tline = TranslateConsoleLine(wline); free(wline); if (tline) { ConsolePutW(tline); ConsolePutW(L"\r\n"); free(tline);} }
                                        start = nl ? nl + 1 : end;
                                    }
                                }
                                if (redirect.buffer) free(redirect.buffer);
                            } else {
                                PPFShowPatchInfo();
                            }
                            _close(ppf);
                            ConsolePutW(tw("done")); ConsolePutW(L"\r\n"); fflush(stdout);
                        }
                    }
                } else if (argv[1][0] == 'f') {
                    if (argc < 4) {
                        ConsolePutW(tw("usage_f_addfileid"));
                    } else {
                        ppf = _open(argv[2], _O_BINARY | _O_RDWR);
                        fileid = _open(argv[3], _O_RDONLY | _O_BINARY);
                        if (ppf == -1 || fileid == -1) {
                            ConsolePutW(tw("error_cannot_open_files"));
                        } else if (!CheckIfPPF3()) {
                            ConsolePrintfKey("error_not_ppf3", argv[2]);
                        } else if (!CheckIfFileId()) {
                            PPFAddFileId();
                            ConsolePutW(tw("fileid_added"));
                        } else {
                            ConsolePutW(tw("error_patch_has_fileid"));
                        }
                        CloseAllFiles();
                    }
                }
            }
            // --- ApplyPPF modo consola ---
            if (!handled && (argv[1][0] == 'a' || argv[1][0] == 'u')) {
                extern int OpenFiles(char*, char*);
                extern int PPFVersion(int ppf);
                extern void ApplyPPF1Patch(int ppf, int bin);
                extern void ApplyPPF2Patch(int ppf, int bin);
                extern void ApplyPPF3Patch(int ppf, int bin, char mode);
                /* Finalize helper (replaces temp->original when ApplyPPF is called in-process) */
                extern void ApplyPPF_Finalize(void);
                /* Expose success status from ApplyPPF so console flow can report failures */
                extern int ApplyPPF_GetSuccess(void);
                extern int ppf, bin;
                #define APPLY 1
                #define UNDO 2
                if (argc != 4) {
                    ConsolePutW(tw("usage_apply"));
                } else {
                    if (OpenFiles(argv[2], argv[3])) {
                        ConsolePutW(tw("error_could_not_open_files_apply"));
                    } else {
                        int x = PPFVersion(ppf);
                        if (argv[1][0] == 'a') {
                            /* Capture and translate ApplyPPF output so messages appear localized in console mode */
                            StdoutRedirect redirect; if (RedirectStdout(&redirect)) {
                                if (x == 1) { ApplyPPF1Patch(ppf, bin); }
                                else if (x == 2) { ApplyPPF2Patch(ppf, bin); }
                                else if (x == 3) { ApplyPPF3Patch(ppf, bin, APPLY); }
                                RestoreStdout(&redirect);
                                if (redirect.buffer && redirect.buffer[0]) {
                                    char *start = redirect.buffer; char *end = redirect.buffer + redirect.buffer_size;
                                    while (start < end) {
                                        char *nl = (char*)memchr(start, '\n', end - start);
                                        size_t linelen = nl ? (size_t)(nl - start + 1) : (size_t)(end - start);
                                        if (linelen == 0) { start = nl ? nl + 1 : end; continue; }
                                        char linebuf[4096]; if (linelen >= sizeof(linebuf)) linelen = sizeof(linebuf)-1;
                                        memcpy(linebuf, start, linelen); linebuf[linelen]=0;
                                        size_t real_len = linelen; while (real_len > 0 && (linebuf[real_len-1]=='\n' || linebuf[real_len-1]=='\r')) linebuf[--real_len]=0;
                                        int wlen = MultiByteToWideChar(CP_UTF8, 0, linebuf, -1, NULL, 0);
                                        wchar_t *wline = NULL; if (wlen <= 0) { wlen = MultiByteToWideChar(CP_ACP, 0, linebuf, -1, NULL, 0); if (wlen>0) { wline=(wchar_t*)malloc(wlen*sizeof(wchar_t)); MultiByteToWideChar(CP_ACP,0,linebuf,-1,wline,wlen);} } else { wline=(wchar_t*)malloc(wlen*sizeof(wchar_t)); MultiByteToWideChar(CP_UTF8,0,linebuf,-1,wline,wlen);} 
                                        if (wline) { wchar_t *tline = TranslateConsoleLine(wline); free(wline); if (tline) { ConsolePutW(tline); ConsolePutW(L"\r\n"); free(tline);} }
                                        start = nl ? nl + 1 : end;
                                    }
                                }
                                if (redirect.buffer) free(redirect.buffer);
                            } else {
                                /* fallback: direct invocation when redirection unavailable */
                                if (x == 1) { ApplyPPF1Patch(ppf, bin); }
                                else if (x == 2) { ApplyPPF2Patch(ppf, bin); }
                                else if (x == 3) { ApplyPPF3Patch(ppf, bin, APPLY); }
                            }
                            if (ApplyPPF_GetSuccess()) ConsolePutW(tw("ppf3_applied")); else ConsolePutW(tw("ppf_apply_failed"));
                        } else if (argv[1][0] == 'u') {
                            StdoutRedirect redirect2; if (RedirectStdout(&redirect2)) {
                                if (x == 3) { ApplyPPF3Patch(ppf, bin, UNDO); }
                                RestoreStdout(&redirect2);
                                if (redirect2.buffer && redirect2.buffer[0]) {
                                    char *start = redirect2.buffer; char *end = redirect2.buffer + redirect2.buffer_size;
                                    while (start < end) {
                                        char *nl = (char*)memchr(start, '\n', end - start);
                                        size_t linelen = nl ? (size_t)(nl - start + 1) : (size_t)(end - start);
                                        if (linelen == 0) { start = nl ? nl + 1 : end; continue; }
                                        char linebuf[4096]; if (linelen >= sizeof(linebuf)) linelen = sizeof(linebuf)-1;
                                        memcpy(linebuf, start, linelen); linebuf[linelen]=0;
                                        size_t real_len = linelen; while (real_len > 0 && (linebuf[real_len-1]=='\n' || linebuf[real_len-1]=='\r')) linebuf[--real_len]=0;
                                        int wlen = MultiByteToWideChar(CP_UTF8, 0, linebuf, -1, NULL, 0);
                                        wchar_t *wline = NULL; if (wlen <= 0) { wlen = MultiByteToWideChar(CP_ACP, 0, linebuf, -1, NULL, 0); if (wlen>0) { wline=(wchar_t*)malloc(wlen*sizeof(wchar_t)); MultiByteToWideChar(CP_ACP,0,linebuf,-1,wline,wlen);} } else { wline=(wchar_t*)malloc(wlen*sizeof(wchar_t)); MultiByteToWideChar(CP_UTF8,0,linebuf,-1,wline,wlen);} 
                                        if (wline) { wchar_t *tline = TranslateConsoleLine(wline); free(wline); if (tline) { ConsolePutW(tline); ConsolePutW(L"\r\n"); free(tline);} }
                                        start = nl ? nl + 1 : end;
                                    }
                                }
                                if (redirect2.buffer) free(redirect2.buffer);
                            } else {
                                if (x == 3) { ApplyPPF3Patch(ppf, bin, UNDO); }
                            }
                            if (ApplyPPF_GetSuccess()) ConsolePutW(tw("ppf3_undo_applied")); else ConsolePutW(tw("ppf_apply_failed"));
                        }
                        /* Finalize temp file replacement/cleanup so in-process apply mirrors standalone behavior */
                        ApplyPPF_Finalize();
                    }
                }
                handled = 1;
            }
            if (!handled) {
                // Ayuda general
                ConsolePutW(tw("console_help"));
            }
        } else {
            // Sin argumentos pero en consola: mostrar ayuda y salir
            ConsolePutW(tw("console_help"));
        }
        for (int i = 0; i < argc; ++i) free(argv[i]);
        free(argv);
        // Forzar flush de stdout y stderr
        fflush(stdout);
        fflush(stderr);
        // No inyectar teclas: solo asegurar que el cursor queda en una línea nueva.
        ConsoleEnsureNewline();
        LocalFree(argvW);
        if (!had_console_at_start) FreeConsole();
        return 0;
    }

    if (argvW) LocalFree(argvW);
    // GUI mode: make sure stdout/stderr aren't left pointing at a released console.
    EnsureGuiStdioReady();
    /* Try to set process DPI awareness to Per Monitor v2 if supported (do this BEFORE creating any windows) */
    typedef BOOL (WINAPI *SetProcessDpiAwarenessContext_t)(void*);
    SetProcessDpiAwarenessContext_t pSetProcessDpiAwarenessContext = (SetProcessDpiAwarenessContext_t)GetProcAddress(GetModuleHandleW(L"user32"), "SetProcessDpiAwarenessContext");
    if (pSetProcessDpiAwarenessContext) {
        /* Per-monitor v2 context value is (DPI_AWARENESS_CONTEXT)-4 on supported OS */
        pSetProcessDpiAwarenessContext((void*)(INT_PTR)-4);
    }

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(icex);
    /* Ensure progress control class is initialized as well */
    icex.dwICC = ICC_STANDARD_CLASSES | ICC_TAB_CLASSES | ICC_WIN95_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icex);

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    /* Load embedded icon resource (resource ID 101) using system DPI if available */
    int sysdpi = GetSystemDPI();
    int cxIcon = GetSystemMetricsForDpi(SM_CXICON, sysdpi);
    int cyIcon = GetSystemMetricsForDpi(SM_CYICON, sysdpi);
    int cxSm = GetSystemMetricsForDpi(SM_CXSMICON, sysdpi);
    int cySm = GetSystemMetricsForDpi(SM_CYSMICON, sysdpi);
    wc.hIcon = LoadIconWithScaleDownIfAvailable(hInstance, MAKEINTRESOURCEW(101), cxIcon, cyIcon);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    wc.lpszClassName = L"PPFManagerClass";
    /* Small icon (titlebar/taskbar) */
    wc.hIconSm = LoadIconWithScaleDownIfAvailable(hInstance, MAKEINTRESOURCEW(101), cxSm, cySm);
    RegisterClassExW(&wc);

    int base_w = 545, base_h = 645; // Tamaño ventana a 96 DPI

    /* Create window at base logical size, then resize it to the window's DPI after creation
       (this ensures the size is calculated using the monitor DPI where the window is created) */
    HWND hwnd = CreateWindowW(L"PPFManagerClass", L"PPF Manager", WS_OVERLAPPEDWINDOW & ~WS_BORDER,
        CW_USEDEFAULT, CW_USEDEFAULT, base_w, base_h, NULL, NULL, hInstance, NULL);
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    /* After creation, query the window DPI and apply scaled dimensions */
    int dpiInit = GetWindowDPI(hwnd);
    int scaled_w = MulDiv(base_w, dpiInit, 96);
    int scaled_h = MulDiv(base_h, dpiInit, 96);
    SetWindowPos(hwnd, NULL, 0, 0, scaled_w, scaled_h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    /* Load and set proper-sized icons for the window (helps on high-DPI displays) */
    LoadAndSetIconsForDPI(hwnd, dpiInit);

    /* Force initial layout recalculation */
    SendMessageW(hwnd, WM_SIZE, 0, 0);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

// Console-subsystem entry point (MinGW -mconsole -municode).
int wmain(int argc, wchar_t **argv) {
    (void)argc;
    (void)argv;
    return wWinMain(GetModuleHandleW(NULL), NULL, GetCommandLineW(), SW_SHOWNORMAL);
}

