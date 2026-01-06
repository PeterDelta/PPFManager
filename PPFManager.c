#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <io.h>
#include <fcntl.h>
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
static int GetSystemMetricsForDpiSafe(int index, UINT dpi) {
    typedef int (WINAPI *GetSystemMetricsForDpi_t)(int, UINT);
    GetSystemMetricsForDpi_t pGetSystemMetricsForDpi = (GetSystemMetricsForDpi_t)GetProcAddress(GetModuleHandleW(L"user32"), "GetSystemMetricsForDpi");
    if (pGetSystemMetricsForDpi) return pGetSystemMetricsForDpi(index, dpi);
    return GetSystemMetrics(index);
}

/* Forward declaration for font enum helper */
static BOOL CALLBACK SetFontEnumProc(HWND hwndChild, LPARAM lParam);

/* Load icon using LoadIconWithScaleDown if available (preserves alpha when scaling down) */
static HICON LoadIconWithScaleDownIfAvailable(HINSTANCE hInst, LPCWSTR name, int cx, int cy) {
    typedef HRESULT (WINAPI *LoadIconWithScaleDown_t)(HINSTANCE, PCWSTR, int, int, HICON*);
    LoadIconWithScaleDown_t pLoadIconWithScaleDown = (LoadIconWithScaleDown_t)GetProcAddress(GetModuleHandleW(L"user32"), "LoadIconWithScaleDown");
    HICON hIcon = NULL;
    if (pLoadIconWithScaleDown) {
        if (SUCCEEDED(pLoadIconWithScaleDown(hInst, name, cx, cy, &hIcon))) return hIcon;
    }
    // Fallback: try LoadImage with exact size, then default sizes
    hIcon = (HICON)LoadImageW(hInst, name, IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR);
    if (hIcon) return hIcon;
    hIcon = (HICON)LoadImageW(hInst, name, IMAGE_ICON, 0, 0, LR_DEFAULTCOLOR | LR_DEFAULTSIZE);
    return hIcon;
}

/* Load icon resource at sizes scaled to `dpi` and set them on the window. Destroys previous icons if present. */
static void LoadAndSetIconsForDPI(HWND hwnd, int dpi) {
    HINSTANCE hInst = GetModuleHandleW(NULL);
    int bigSize = GetSystemMetricsForDpiSafe(SM_CXICON, dpi);
    int smallSize = GetSystemMetricsForDpiSafe(SM_CXSMICON, dpi);
    if (g_hIconBig) { DestroyIcon(g_hIconBig); g_hIconBig = NULL; }
    if (g_hIconSmall) { DestroyIcon(g_hIconSmall); g_hIconSmall = NULL; }

    g_hIconBig = LoadIconWithScaleDownIfAvailable(hInst, MAKEINTRESOURCEW(101), bigSize, bigSize);
    if (!g_hIconBig) g_hIconBig = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON, bigSize, bigSize, LR_DEFAULTCOLOR | LR_DEFAULTSIZE);

    g_hIconSmall = LoadIconWithScaleDownIfAvailable(hInst, MAKEINTRESOURCEW(101), smallSize, smallSize);
    if (!g_hIconSmall) g_hIconSmall = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON, smallSize, smallSize, LR_DEFAULTCOLOR | LR_DEFAULTSIZE);

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

// Show Save dialog using IFileSaveDialog with initial folder; return TRUE and filename in outFilename on success
static BOOL ShowSaveFileDialog_COM(HWND owner, wchar_t *outFilename, size_t outSize, const wchar_t *initialDir, const wchar_t *filter) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) return FALSE;
    IFileSaveDialog *pfd = NULL;
    hr = CoCreateInstance(&CLSID_FileSaveDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileSaveDialog, (void**)&pfd);
    if (FAILED(hr)) { CoUninitialize(); return FALSE; }
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
static BOOL ShowOpenFileDialog_COM(HWND owner, wchar_t *outFilename, size_t outSize, const wchar_t *initialDir, const wchar_t *filter) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) return FALSE;
    IFileOpenDialog *pfd = NULL;
    hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void**)&pfd);
    if (FAILED(hr)) { CoUninitialize(); return FALSE; }
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
    hr = pfd->lpVtbl->Show(pfd, owner);
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

// Idioma actual de la UI: 0=ES, 1=EN
enum { LANG_ES = 0, LANG_EN = 1 };
static int g_lang = LANG_ES;

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

static void ApplyMenuTheme(HMENU hMenuBar, bool dark) {
    if (!hMenuBar) return;
    MENUINFO mi = {0};
    mi.cbSize = sizeof(mi);
    mi.fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS | MIM_STYLE;
    mi.dwStyle |= MNS_CHECKORBMP;
    mi.hbrBack = dark ? g_brBg : NULL;
    SetMenuInfo(hMenuBar, &mi);
    int cnt = GetMenuItemCount(hMenuBar);
    for (int i = 0; i < cnt; ++i) {
        HMENU hSub = GetSubMenu(hMenuBar, i);
        if (!hSub) continue;
        SetMenuInfo(hSub, &mi);
    }
}

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
    CheckMenuItem(hMenuIdioma, 201, MF_BYCOMMAND | (g_lang == LANG_ES ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hMenuIdioma, 202, MF_BYCOMMAND | (g_lang == LANG_EN ? MF_CHECKED : MF_UNCHECKED));
}

static void UpdatePanelEdge(HWND panel, bool dark) {
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
    UpdatePanelEdge(hCrearPanel, dark);
    UpdatePanelEdge(hAplicarPanel, dark);
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
// Globals for controls accessible from other functions
static HWND g_hwndTab = NULL;
static HWND g_hCrearPanel = NULL;
static HWND g_hAplicarPanel = NULL;
LRESULT CALLBACK CrearPanelProc(HWND hwndPanel, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK AplicarPanelProc(HWND hwndPanel, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK TabProc(HWND hwndTab, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK ComboProc(HWND hwndCombo, UINT msg, WPARAM wParam, LPARAM lParam);
static void ForceLayoutRefresh(void);
LRESULT CALLBACK AboutWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);


// --- TRADUCCIÓN CENTRALIZADA ---

// Show About dialog (modal, centered on parent)
static void ShowAboutDialog(HWND hwnd) {
    const wchar_t *title = L"PPF Manager";
    const wchar_t *text_es = L"\nPPF Manager vpre-1.0 por PeterDelta\r\nBasado en fuentes PPF3 de Icarus/Paradox\r\n\r\nhttps://github.com/PeterDelta/MakePPF3";
    const wchar_t *text_en = L"\nPPF Manager vpre-1.0 by PeterDelta\r\nBased on PPF3 sources by Icarus/Paradox\r\n\r\nhttps://github.com/PeterDelta/MakePPF3";
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
                                                   GetSystemMetricsForDpiSafe(SM_CXICON, sysdpi_local),
                                                   GetSystemMetricsForDpiSafe(SM_CYICON, sysdpi_local));
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
    if (x < 0) x = 0; if (y < 0) y = 0;

    HWND hDlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE, L"PPFManagerAboutClass", title,
                                 WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                 x, y, dlgW, dlgH, hwnd, NULL, GetModuleHandleW(NULL), (LPVOID)text);
    if (hDlg) {
        // Put app icon on the title bar for About
        HINSTANCE hInst = GetModuleHandleW(NULL);
        int sysdpi_ab = GetSystemDPI();
        HICON hBig = LoadIconWithScaleDownIfAvailable(hInst, MAKEINTRESOURCEW(101),
                                                      GetSystemMetricsForDpiSafe(SM_CXICON, sysdpi_ab),
                                                      GetSystemMetricsForDpiSafe(SM_CYICON, sysdpi_ab));
        HICON hSmall = LoadIconWithScaleDownIfAvailable(hInst, MAKEINTRESOURCEW(101),
                                                        GetSystemMetricsForDpiSafe(SM_CXSMICON, sysdpi_ab),
                                                        GetSystemMetricsForDpiSafe(SM_CYSMICON, sysdpi_ab));
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
    if (strcmp(key, "controls_dbg") == 0)
        return (g_lang == LANG_EN)
            ? L"Controls: hCrearEditImg=%p hCrearBtnImg=%p hCrearEditMod=%p hCrearBtnMod=%p hCrearBtnPPF=%p"
            : L"Controles: hCrearEditImg=%p hCrearBtnImg=%p hCrearEditMod=%p hCrearBtnMod=%p hCrearBtnPPF=%p";
    if (strcmp(key, "wmcmd_dbg") == 0)
        return (g_lang == LANG_EN)
            ? L"WM_COMMAND received: id=%d notif=%d"
            : L"WM_COMMAND recibido: id=%d notif=%d";
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
            : L"Ejecutar: %s";
    if (strcmp(key, "select_ppf_info") == 0)
        return (g_lang == LANG_EN)
            ? L"Select a PPF file to view its information.\r\n"
            : L"Selecciona un archivo PPF para ver la información.\r\n";
    if (strcmp(key, "select_ppf_fileid") == 0)
        return (g_lang == LANG_EN)
            ? L"Select a PPF and file_id.diz to add.\r\n"
            : L"Selecciona PPF y file_id.diz para añadir.\r\n";
    return L"";
}

typedef struct { const wchar_t *id; const wchar_t *es; const wchar_t *en; } UI_TEXT_ENTRY;
static const UI_TEXT_ENTRY UI_TEXTS[] = {
    {L"tab_create", L" Crear Parche ", L" Create Patch "},
    {L"tab_apply", L" Aplicar Parche ", L" Apply Patch "},
    {L"menu_lang", L"Idioma", L"Language"},
    {L"menu_theme", L"Tema", L"Theme"},
    {L"menu_help", L"Ayuda", L"Help"},
    {L"menu_about", L"Acerca de", L"About"},
    {L"menu_es", L"Español", L"Spanish"},
    {L"menu_en", L"Inglés", L"English"},
    {L"menu_dark", L"Oscuro", L"Dark"},
    {L"menu_light", L"Claro", L"Light"},
    {L"lbl_img", L"Imagen original:", L"Original image:"},
    {L"lbl_mod", L"Imagen modificada:", L"Modified image:"},
    {L"lbl_ppf_dest", L"Archivo PPF destino:", L"Output PPF file:"},
    {L"lbl_diz", L"File_id.diz (opcional):", L"File_id.diz (optional):"},
    {L"lbl_desc", L"Descripción (opcional):", L"Description (optional):"},
    {L"chk_undo", L"Incluir datos de deshacer", L"Include undo data"},
    {L"chk_valid", L"Activar validación", L"Enable validation"},
    {L"lbl_tipo", L"Imagen:", L"Image:"},
    {L"btn_create", L"Crear Parche", L"Create Patch"},
    {L"btn_show", L"Info Parche", L"Patch Info"},
    {L"btn_add", L"Añadir file_id", L"Add file_id.diz"},
    {L"btn_clear", L"Limpiar", L"Clear"},
    {L"lbl_salida", L"Salida:", L"Output:"},
    {L"lbl_img_apply", L"Imagen original:", L"Original image:"},
    {L"lbl_ppf_apply", L"Archivo PPF:", L"PPF file:"},
    {L"chk_revert", L"Deshacer parche", L"Undo patch"},
    {L"btn_apply", L"Aplicar Parche", L"Apply Patch"},
    {L"btn_clear_apply", L"Limpiar", L"Clear"},
    {L"lbl_salida_apply", L"Salida:", L"Output:"}
};
static const wchar_t* T(const wchar_t* id) {
    for (int i = 0; i < sizeof(UI_TEXTS)/sizeof(UI_TEXTS[0]); ++i) {
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
    ModifyMenuW(hMenuIdioma, 0, MF_BYPOSITION | MF_STRING, 201, T(L"menu_es"));
    ModifyMenuW(hMenuIdioma, 1, MF_BYPOSITION | MF_STRING, 202, T(L"menu_en"));
    ModifyMenuW(hMenuTema, 0, MF_BYPOSITION | MF_STRING, 203, T(L"menu_dark"));
    ModifyMenuW(hMenuTema, 1, MF_BYPOSITION | MF_STRING, 204, T(L"menu_light"));
    ModifyMenuW(hMenuAyuda, 0, MF_BYPOSITION | MF_STRING, 206, T(L"menu_about"));
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
    // type: BIN=0, GI=1 -> use -i <0|1>
    if (hComboTipo) {
        int sel = (int)SendMessageW(hComboTipo, CB_GETCURSEL, 0, 0);
        int itype = (sel == 1) ? 1 : 0;
        wchar_t itbuf[8];
        _snwprintf(itbuf, sizeof(itbuf)/sizeof(wchar_t), L"%d", itype);
        wcscat_s(out, outSize, L" -i ");
        AppendQuotedArg(out, outSize, itbuf);
    }
    if (hDesc && GetWindowTextW(hDesc, buf, MAX_PATH) && wcslen(buf) > 0) {
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

// Logging control: disable by default to avoid heavy file output
static int ppfmanager_log_enabled = 0;

// Log a message to a workspace file for post-mortem inspection
static void LogToFile(const wchar_t *msg) {
    if (!msg) return;
    if (!ppfmanager_log_enabled) return; /* logging disabled */
    // convert to UTF-8
    int needed = WideCharToMultiByte(CP_UTF8, 0, msg, -1, NULL, 0, NULL, NULL);
    if (needed <= 0) return;
    char *buf = (char*)malloc(needed);
    if (!buf) return; /* CRITICAL: Check malloc failure */
    WideCharToMultiByte(CP_UTF8, 0, msg, -1, buf, needed, NULL, NULL);
    FILE *f = fopen("ppfmanager.log", "a");
    if (f) {
        fprintf(f, "%s\n", buf);
        fclose(f);
    }
    free(buf);
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
    _snwprintf(folder, MAX_PATH, L"%s\\MakePPF", appdata);
    CreateDirectoryW(folder, NULL);
    _snwprintf(out, outSize, L"%s\\settings.ini", folder);
}

// Load settings from INI and populate controls (tolerant)
static void LoadSettings(HWND hCrearEditImg, HWND hCrearEditMod, HWND hCrearEditPPF, HWND hCrearEditDIZ, HWND hCrearEditDesc, HWND hCrearChkUndo, HWND hCrearChkValid, HWND hCrearComboTipo,
                         HWND hAplicarEditImg, HWND hAplicarEditPPF, HWND hAplicarChkRevert) {
    wchar_t inipath[MAX_PATH];
    GetSettingsFilePath(inipath, MAX_PATH);
    wchar_t buf[MAX_PATH];
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
                    // Tamaño de ventana por defecto (aplicar posición restaurada)
                    SetWindowPos(g_hwndMain, NULL, left, top,545, 670, SWP_NOZORDER | SWP_NOACTIVATE);
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
    GetSettingsFilePath(inipath, MAX_PATH);
    wchar_t buf[MAX_PATH];
    // Do NOT save path fields or control/check states. Only save window position on exit.
    if (g_hwndMain) {
        // Prefer the normal (restored) position so we don't persist minimized coordinates
        WINDOWPLACEMENT wp = {0}; wp.length = sizeof(wp);
        if (GetWindowPlacement(g_hwndMain, &wp)) {
            RECT rc = wp.rcNormalPosition;
            int left = rc.left; int top = rc.top;
            wchar_t sbuf[64];
            _snwprintf(sbuf, 64, L"%d", left); WritePrivateProfileStringW(L"Window", L"Left", sbuf, inipath);
            _snwprintf(sbuf, 64, L"%d", top); WritePrivateProfileStringW(L"Window", L"Top", sbuf, inipath);
            WritePrivateProfileStringW(L"Window", L"HasPos", L"1", inipath);
        } else {
            // fallback: use current window rect if placement failed
            RECT rc; GetWindowRect(g_hwndMain, &rc);
            int left = rc.left; int top = rc.top;
            wchar_t sbuf[64];
            _snwprintf(sbuf, 64, L"%d", left); WritePrivateProfileStringW(L"Window", L"Left", sbuf, inipath);
            _snwprintf(sbuf, 64, L"%d", top); WritePrivateProfileStringW(L"Window", L"Top", sbuf, inipath);
            WritePrivateProfileStringW(L"Window", L"HasPos", L"1", inipath);
        }
    }
    // Save language selection
    {
        wchar_t lbuf[4];
        _snwprintf(lbuf, 4, L"%d", (g_lang == LANG_EN) ? 1 : 0);
        WritePrivateProfileStringW(L"Window", L"LangEn", lbuf, inipath);
    }
    // Save theme preference (0 claro, 1 oscuro)
    {
        wchar_t tbuf[4];
        _snwprintf(tbuf, 4, L"%d", g_themePref ? 1 : 0);
        WritePrivateProfileStringW(L"Window", L"ThemeDark", tbuf, inipath);
    }
}

// Append Unicode text to an edit control (must be called from UI thread)
static void AppendTextToEdit(HWND hEdit, const wchar_t *text) {
    if (!IsWindow(hEdit) || !text) return;
    // set selection to end
    SendMessageW(hEdit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
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

// Translate a single console line to Spanish if appropriate. Returns heap-allocated wide string (caller must free).
static wchar_t* TranslateConsoleLine(const wchar_t *line) {
    if (!line) return NULL;
    if (g_lang != LANG_ES) return _wcsdup(line);

    // Start with a copy we can modify
    size_t bufSize = wcslen(line) + 256;
    wchar_t *buf = (wchar_t*)malloc(bufSize * sizeof(wchar_t));
    if (!buf) return _wcsdup(line);
    wcscpy(buf, line);

    // Simple substring replacements (order matters for idempotence)
    struct { const wchar_t *find; const wchar_t *repl; } rules[] = {
        // MakePPF common
        { L"Writing header...", L"Escribiendo cabecera..." },
        { L"Adding file_id.diz...", L"Añadiendo file_id.diz..." },
        { L"Finding differences...", L"Buscando diferencias..." },
        { L"Progress:", L"Progreso:" },
        { L"entries found", L"entradas encontradas" },
        { L"entries", L"entradas" },
        { L"Version", L"Versión" },
        { L"Enc.Method", L"Método Enc." },
        { L"Imagetype", L"Tipo de imagen" },
        { L"Validation", L"Validación" },
        { L"Undo Data", L"Datos Deshacer" },
        { L"Description", L"Descripción" },
        { L"File.id_diz", L"File.id_diz" },
        { L"Error: insufficient memory available", L"Error: memoria insuficiente disponible" },
        { L"Error: filesize of bin file is zero!", L"Error: el tamaño del archivo bin es cero!" },
        { L"Error: bin files are different in size.", L"Error: los archivos bin tienen distinto tamaño." },
        { L"Error: need more input for command", L"Error: falta entrada para el comando" },
        { L"Error: cannot open file \"", L"Error: no se puede abrir el archivo \"" },
        { L"Error: cannot open file ", L"Error: no se puede abrir el archivo " },
        { L"Error: file ", L"Error: el archivo " },
        { L"Error: patch already contains a file_id.diz", L"Error: el parche ya contiene file_id.diz" },
        { L"Error: cannot create temp file for", L"Error: no se puede crear archivo temporal para" },
        { L"Showing patchinfo", L"Mostrando información del parche" },
        { L"Done.", L"Finalizado." },
        { L"done.", L"Hecho." },
        { L"Enabled", L"Habilitado." },
        { L"Disabled", L"Deshabilitado." },
        { L"No such file or directory", L"El archivo o directorio no existe." },
        // ApplyPPF common (fallbacks in case English output is produced)
        { L"Patching...", L"Parcheando..." },
        { L"Patching ...", L"Parcheando ..." },
        { L"reading...", L"leyendo..." },
        { L"writing...", L"escribiendo..." },
        { L"successful.", L"Finalizado." },
        { L"Patch Information:", L"Información del parche:" },
        { L"Patchfile is a PPF3.0 patch.", L"El archivo es un parche PPF3.0." },
        { L"Not available", L"No disponible" },
        { L"Available", L"Disponible" },
        { L"Unknown command", L"Comando desconocido" },
        { L"unknown command", L"Comando desconocido" },
        { L"Executing:", L"Ejecutando:" },
        { L"Execute:", L"Ejecutar:" },
        { L"Done.", L"Finalizado." },
        { L"Usage: PPF <command> [-<sw> [-<sw>...]] <original bin> <modified bin> <ppf>", L"Uso: PPF <comando> [-<sw> [-<sw>...]] <Imagen original> <Imagen modificado> <ppf>" },
        { L"<Commands>", L"<Comandos>" },
        { L"  c : create PPF3.0 patch            a : add file_id.diz", L"  c : crear parche PPF3.0            a : añadir file_id.diz" },
        { L"  s : show patchinfomation", L"  s : mostrar información del parche" },
        { L"<Switches>", L"<Interruptores>" },
        { L" -u        : include undo data (default=off)", L" -u        : incluir datos de deshacer (por defecto=apagado)" },
        { L" -x        : disable patchvalidation (default=off)", L" -x        : deshabilitar validación de parche (por defecto=apagado)" },
        { L" -i [0/1]  : imagetype, 0 = BIN, 1 = GI (default=bin)", L" -i [0/1]  : tipo de imagen, 0 = BIN, 1 = GI (por defecto=bin)" },
        { L" -d \"text\" : use \"text\" as description", L" -d \"texto\" : usar \"texto\" como descripción" },
        { L" -f \"file\" : add \"file\" as file_id.diz", L" -f \"archivo\" : añadir \"archivo\" como file_id.diz" },
        { L"Examples: PPF c -u -i 1 -d \"my elite patch\" game.bin patch.bin output.ppf", L"Ejemplos: PPF c -u -i 1 -d \"mi parche elite\" juego.bin parche.bin salida.ppf" },
        { L"          PPF a patch.ppf myfileid.txt", L"          PPF a patch.ppf fileid.txt" },
        { L"Usage: ApplyPPF <command> <binfile> <patchfile>", L"Uso: ApplyPPF <comando> <archivo bin> <archivo parche>" },
        { L"  a : apply PPF1/2/3 patch", L"  a : aplicar parche PPF1/2/3" },
        { L"  u : undo patch (PPF3 only)", L"  u : deshacer parche (solo PPF3)" },
        { L"Example: ApplyPPF.exe a game.bin patch.ppf", L"Ejemplo: ApplyPPF.exe a juego.bin parche.ppf" },
    };

    // Apply each rule conservatively using ReplaceAllWide
    for (size_t i = 0; i < sizeof(rules)/sizeof(rules[0]); ++i) {
        wchar_t *r = ReplaceAllWide(buf, rules[i].find, rules[i].repl);
        if (r) {
            free(buf);
            buf = r;
        }
    }

    // If MakePPF reports missing arguments for command 'c', show the full help instead of the raw error.
    if (wcsstr(line, L"Error: need more input for command") != NULL || wcsstr(buf, L"Error: falta entrada para el comando") != NULL) {
        const wchar_t *help_en = L"Usage: PPF <command> [-<sw> [-<sw>...]] <original bin> <modified bin> <ppf>\r\n"
            L"<Commands>\r\n"
            L"  c : create PPF3.0 patch            a : add file_id.diz\r\n"
            L"  s : show patchinfomation\r\n"
            L"<Switches>\r\n"
            L" -u        : include undo data\r\n"
            L" -x        : disable patchvalidation\r\n"
            L" -i [0/1]  : imagetype, 0 = BIN, 1 = GI\r\n"
            L" -d \"text\" : use \"text\" as description\r\n"
            L" -f \"file\" : add \"file\" as file_id.diz\r\n\r\n"
            L"Examples: PPF c -u -i 1 -d game.bin patch.bin output.ppf\r\n"
            L"          PPF a patch.ppf myfileid.txt\r\n";
        const wchar_t *help_es = L"Uso: PPF <comando> [-<sw> [-<sw>...]] <Imagen original> <Imagen modificada> <ppf>\r\n"
            L"<Comandos>\r\n"
            L"  c : crear parche PPF3.0      a : añadir file_id.diz\r\n"
            L"  s : mostrar información del parche\r\n"
            L"<opciones>\r\n"
            L" -u        : incluir datos de deshacer\r\n"
            L" -x        : deshabilitar validación de parche\r\n"
            L" -i [0/1]  : tipo de imagen, 0 = BIN, 1 = GI)\r\n"
            L" -d \"texto\" : usar \"texto\" como descripción\r\n"
            L" -f \"archivo\" : añadir \"archivo\" como file_id.diz\r\n\r\n"
            L"Ejemplos: PPF c -u -i 1 -d juego.bin parche.bin salida.ppf\r\n"
            L"          PPF a patch.ppf fileid.txt\r\n";
        const wchar_t *help = (g_lang == LANG_EN) ? help_en : help_es;
        wchar_t *r = _wcsdup(help);
        free(buf);
        return r;
    }

    // Additional small pattern: Progress: xx.xx% (N entries found) -> Progreso: xx.xx% (N entradas encontradas)
    // This may already be covered by substrings but ensure 'entries found' translated.

    // Align colon for several known key/value lines so colons are vertically aligned in monospaced output
    {
        const wchar_t* align_keys[] = {
            L"Version", L"Versión",
            L"Enc.Method", L"Método Enc.",
            L"Imagetype", L"Tipo de imagen",
            L"Validation", L"Validación",
            L"Undo Data", L"Datos Deshacer",
            L"Description", L"Descripción",
            L"File.id_diz"
        };
        const int align_width = 15; // desired minimum key column width
        // find first non-space char
        const wchar_t *p = buf;
        while (*p == L' ') p++;
        for (size_t i = 0; i < sizeof(align_keys)/sizeof(align_keys[0]); ++i) {
            const wchar_t *k = align_keys[i];
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
        }
    }

    return buf;
}

// Helper to redirect stdout to a temporary file for capturing output
typedef struct {
    FILE *old_stdout;
    FILE *old_stderr;
    char temp_filename[MAX_PATH];
    char *buffer;
    size_t buffer_size;
} StdoutRedirect;

static int RedirectStdout(StdoutRedirect *redirect) {
    redirect->buffer = NULL;
    redirect->buffer_size = 0;
    
    // Create temp file
    char temp_path[MAX_PATH];
    GetTempPathA(MAX_PATH, temp_path);
    GetTempFileNameA(temp_path, "ppf", 0, redirect->temp_filename);
    
    // Save old stdout/stderr
    redirect->old_stdout = stdout;
    redirect->old_stderr = stderr;
    
    // Redirect stdout and stderr to temp file
    FILE *temp_file = freopen(redirect->temp_filename, "w", stdout);
    if (!temp_file) return 0;
    freopen(redirect->temp_filename, "w", stderr);
    
    // Use line buffering for better capture
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    
    return 1;
}

static void RestoreStdout(StdoutRedirect *redirect) {
    fflush(stdout);
    fflush(stderr);
    
    // Restore stdout/stderr
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
    *stdout = *redirect->old_stdout;
    *stderr = *redirect->old_stderr;
    
    // Read the temp file into buffer
    FILE *f = fopen(redirect->temp_filename, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        
        if (size > 0) {
            redirect->buffer = (char*)malloc(size + 1);
            if (redirect->buffer) {
                redirect->buffer_size = fread(redirect->buffer, 1, size, f);
                redirect->buffer[redirect->buffer_size] = 0;
            }
        }
        fclose(f);
    }
    
    // Delete temp file
    DeleteFileA(redirect->temp_filename);
}

// Integrated execution thread: calls MakePPF/ApplyPPF functions directly
static DWORD WINAPI IntegratedExecutionThread(LPVOID lpParam) {
    PROC_THREAD_PARAM *p = (PROC_THREAD_PARAM*)lpParam;
    
    // Check if another operation is already running
    if (InterlockedCompareExchange(&g_operation_running, 1, 0) != 0) {
        wchar_t *warn = _wcsdup(L"⚠️ Operación ya en curso. Por favor espera a que termine.\r\n\r\n");
        PostMessageW(GetForegroundWindow(), WM_APPEND_OUTPUT, (WPARAM)p->hEdit, (LPARAM)warn);
        free(p);
        return 1;
    }
    
    // Parse command line to extract argc/argv
    wchar_t cmdline[1024];
    wcscpy(cmdline, p->cmdline);
    
    // Convert wide command line to argv array using proper parsing
    char *argv[32];
    int argc = 0;
    static char arg_storage[32][MAX_PATH * 2]; // static to ensure lifetime
    
    wchar_t *ptr = cmdline;
    
    // Skip leading spaces
    while (*ptr && iswspace(*ptr)) ptr++;
    
    while (*ptr && argc < 31) {
        wchar_t arg_buf[MAX_PATH * 2];
        int arg_len = 0;
        int in_quotes = 0;
        
        // Parse one argument
        while (*ptr && (in_quotes || !iswspace(*ptr))) {
            if (*ptr == L'"') {
                in_quotes = !in_quotes;
                ptr++; // Skip the quote character
            } else {
                if (arg_len < MAX_PATH * 2 - 1) {
                    arg_buf[arg_len++] = *ptr;
                }
                ptr++;
            }
        }
        
        // Store the argument
        if (arg_len > 0) {
            arg_buf[arg_len] = 0;
            WideCharToMultiByte(CP_UTF8, 0, arg_buf, -1, arg_storage[argc], MAX_PATH * 2, NULL, NULL);
            argv[argc] = arg_storage[argc];
            argc++;
        }
        
        // Skip trailing spaces
        while (*ptr && iswspace(*ptr)) ptr++;
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
        
        wchar_t *wbuf = (wchar_t*)malloc((wcslen(shortbuf) + 128) * sizeof(wchar_t));
        if (wbuf) {
            wchar_t tmp[2048];
            _snwprintf(tmp, 2048, tw("exec"), shortbuf);
            const wchar_t *sep = L"——————————————————————————————————————————————————";
            swprintf_s(wbuf, (int)(wcslen(sep) + wcslen(tmp) + 8), L"%s\r\n%s\r\n\r\n", sep, tmp);
            PostMessageW(GetForegroundWindow(), WM_APPEND_OUTPUT, (WPARAM)p->hEdit, (LPARAM)wbuf);
        }
        free(shortbuf);
    }
    
    // Redirect stdout to buffer
    StdoutRedirect redirect;
    if (!RedirectStdout(&redirect)) {
        wchar_t *err = _wcsdup(L"Error: Could not redirect stdout\r\n");
        PostMessageW(GetForegroundWindow(), WM_APPEND_OUTPUT, (WPARAM)p->hEdit, (LPARAM)err);
        free(p);
        return 1;
    }
    
    // Determine which command to execute
    int result = 0;
    if (argc > 0) {
        char *exe_name = argv[0];
        
        // CRITICAL: Reset global state before each operation to prevent crashes on repeated executions
        ResetGlobalState();
        
        // Check if it's MakePPF or ApplyPPF
        if (strstr(exe_name, "MakePPF") || strstr(exe_name, "makeppf")) {
            result = MakePPF_Main(argc, argv);
        } else if (strstr(exe_name, "ApplyPPF") || strstr(exe_name, "applyppf")) {
            result = ApplyPPF_Main(argc, argv);
        } else {
            printf("Error: Unknown command\n");
            result = 1;
        }
    }
    
    // Restore stdout
    RestoreStdout(&redirect);
    
    // Process captured output from buffer
    if (redirect.buffer && redirect.buffer[0] != 0) {
        char *start = redirect.buffer;
        char *end = redirect.buffer + strlen(redirect.buffer);
        char *nl = NULL;
        
        // Process lines
        while ((nl = (char*)memchr(start, '\n', end - start)) || start < end) {
            size_t linelen;
            if (nl) {
                linelen = nl - start + 1;
            } else {
                linelen = end - start;
            }
            
            char linebuf[4096];
            if (linelen >= sizeof(linebuf)) linelen = sizeof(linebuf)-1;
            memcpy(linebuf, start, linelen);
            linebuf[linelen] = 0;
            
            // Trim trailing CR/LF
            size_t real_len = linelen;
            while (real_len > 0 && (linebuf[real_len-1] == '\n' || linebuf[real_len-1] == '\r')) {
                linebuf[--real_len] = 0;
            }
            // Convert to wide (handle empty lines too)
            int wlen = MultiByteToWideChar(CP_UTF8, 0, linebuf, -1, NULL, 0);
            wchar_t *wline = NULL;
            if (wlen <= 0) {
                wlen = MultiByteToWideChar(CP_ACP, 0, linebuf, -1, NULL, 0);
                if (wlen > 0) {
                    wline = (wchar_t*)malloc(wlen * sizeof(wchar_t));
                    MultiByteToWideChar(CP_ACP, 0, linebuf, -1, wline, wlen);
                }
            } else {
                wline = (wchar_t*)malloc(wlen * sizeof(wchar_t));
                MultiByteToWideChar(CP_UTF8, 0, linebuf, -1, wline, wlen);
            }

            if (wline) {
                wchar_t *tline = TranslateConsoleLine(wline);
                free(wline);
                if (tline) {
                    size_t tlen = wcslen(tline);
                    wchar_t *wbuf = (wchar_t*)malloc((tlen + 3) * sizeof(wchar_t));
                    if (wbuf) {
                        wcscpy(wbuf, tline);
                        wcscat(wbuf, L"\r\n");
                        PostMessageW(GetForegroundWindow(), WM_APPEND_OUTPUT, (WPARAM)p->hEdit, (LPARAM)wbuf);
                    }
                    free(tline);
                }
            }
            
            if (nl) {
                start = nl + 1;
            } else {
                break;
            }
        }
    }
    
    // Clean up
    if (redirect.buffer) free(redirect.buffer);
    free(p);
    
    // Release operation lock
    InterlockedExchange(&g_operation_running, 0);
    
    return result;
}

// Old thread proc (kept for reference but now unused)
static DWORD WINAPI ProcessCaptureThread_OLD(LPVOID lpParam) {
    PROC_THREAD_PARAM *p = (PROC_THREAD_PARAM*)lpParam;
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    HANDLE hRead = NULL, hWrite = NULL;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        free(p);
        return 1;
    }
    // Ensure read handle not inheritable
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.hStdInput = NULL;
    si.wShowWindow = SW_HIDE;

    // CreateProcess requires mutable command line
    wchar_t cmdline[1024];
    wcscpy(cmdline, p->cmdline);

    /* Build a shortened, more readable command string for display (strip directory paths inside quotes).
       We also add blank lines around it for visual separation. */
    {
        wchar_t shortbuf[2048];
        int si = 0;
        int i = 0;
        /* Extract readable tokens, keeping only filename (no path) inside quotes */
        while (cmdline[i] && si < (int)sizeof(shortbuf)/sizeof(wchar_t) - 1) {
            if (cmdline[i] == L'"') {
                /* copy opening quote */
                shortbuf[si++] = L'"'; i++;
                int start = i;
                while (cmdline[i] && cmdline[i] != L'"') i++;
                int end = i - 1;
                /* find last backslash within quoted token */
                int b = start - 1;
                for (int j = start; j <= end; ++j) if (cmdline[j] == L'\\' || cmdline[j] == L'/') b = j;
                int copy_from = (b >= start) ? (b + 1) : start;
                for (int j = copy_from; j <= end && si < (int)sizeof(shortbuf)/sizeof(wchar_t) - 1; ++j) shortbuf[si++] = cmdline[j];
                if (cmdline[i] == L'"') i++; /* skip closing quote */
                shortbuf[si++] = L'"';
            } else {
                /* copy other characters (space, flags) */
                shortbuf[si++] = cmdline[i++];
            }
        }
        shortbuf[si] = 0;
        /* Build the display string safely using swprintf_s to ensure null-termination */
        wchar_t *wbuf = (wchar_t*)malloc( (wcslen(shortbuf) + 128) * sizeof(wchar_t) );
        if (wbuf) {
            // Use localized prefix for "Execute" messages
            wchar_t tmp[2048]; _snwprintf(tmp, 2048, tw("exec"), shortbuf);
            const wchar_t *sep = L"——————————————————————————————————————————————————";
            // Show a separator line above the command for better readability
            swprintf_s(wbuf, (int)(wcslen(sep) + wcslen(tmp) + 8), L"%s\r\n%s\r\n\r\n", sep, tmp);
            PostMessageW(GetForegroundWindow(), WM_APPEND_OUTPUT, (WPARAM)p->hEdit, (LPARAM)wbuf);
        }
        /* Also write the full command to log for diagnostics */
        {
            wchar_t dbg[2048]; _snwprintf(dbg, 2048, tw("exec"), cmdline);
            LogToFile(dbg);
        }
    }

    BOOL ok = CreateProcessW(NULL, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    // Close write handle in parent so we can read EOF
    CloseHandle(hWrite);
    if (!ok) {
        wchar_t msgbuf[256];
            _snwprintf(msgbuf, 256, tw("createprocess_failed"), GetLastError());
        size_t mlen = wcslen(msgbuf) + 1;
        wchar_t *wbuf = (wchar_t*)malloc(mlen * sizeof(wchar_t));
        if (wbuf) wcscpy(wbuf, msgbuf);
        PostMessageW(GetForegroundWindow(), WM_APPEND_OUTPUT, (WPARAM)p->hEdit, (LPARAM)wbuf);
        CloseHandle(hRead);
        free(p);
        return 1;
    }

    // Read output and process lines (buffered, to handle partial chunks)
    CHAR buffer[512];
    DWORD bytesRead;
    while (ReadFile(hRead, buffer, sizeof(buffer)-1, &bytesRead, NULL) && bytesRead > 0) {
        buffer[bytesRead] = 0;
        // Append to partial buffer (ensure not to overflow)
        int space = (int)sizeof(p->partial) - p->partial_len - 1;
        int tocopy = (int)bytesRead;
        if (tocopy > space) tocopy = space;
        memcpy(p->partial + p->partial_len, buffer, tocopy);
        p->partial_len += tocopy;
        p->partial[p->partial_len] = 0;

        // Process complete lines separated by '\n'
        char *start = p->partial;
        char *end = p->partial + p->partial_len;
        char *nl = NULL;
        while ((nl = (char*)memchr(start, '\n', end - start))) {
            size_t linelen = nl - start + 1; // include newline
            char linebuf[4096];
            if (linelen >= sizeof(linebuf)) linelen = sizeof(linebuf)-1;
            memcpy(linebuf, start, linelen);
            linebuf[linelen] = 0;
            // Trim trailing CR/LF
            size_t real_len = linelen;
            while (real_len > 0 && (linebuf[real_len-1] == '\n' || linebuf[real_len-1] == '\r')) {
                linebuf[--real_len] = 0;
            }

            // Convert to wide (try UTF-8 then fallback to ANSI)
            int wlen = MultiByteToWideChar(CP_UTF8, 0, linebuf, -1, NULL, 0);
            wchar_t *wline = NULL;
            if (wlen <= 0) {
                wlen = MultiByteToWideChar(CP_ACP, 0, linebuf, -1, NULL, 0);
                if (wlen > 0) {
                    wline = (wchar_t*)malloc(wlen * sizeof(wchar_t));
                    MultiByteToWideChar(CP_ACP, 0, linebuf, -1, wline, wlen);
                }
            } else {
                wline = (wchar_t*)malloc(wlen * sizeof(wchar_t));
                MultiByteToWideChar(CP_UTF8, 0, linebuf, -1, wline, wlen);
            }

            if (wline) {
                wchar_t *tline = TranslateConsoleLine(wline);
                free(wline);
                if (tline) {
                    size_t tlen = wcslen(tline);
                    // Append CRLF for display
                    wchar_t *wbuf = (wchar_t*)malloc((tlen + 3) * sizeof(wchar_t));
                    if (wbuf) {
                        wcscpy(wbuf, tline);
                        wcscat(wbuf, L"\r\n");
                        PostMessageW(GetForegroundWindow(), WM_APPEND_OUTPUT, (WPARAM)p->hEdit, (LPARAM)wbuf);
                    }
                    free(tline);
                }
            }

            start = nl + 1;
        }

        // Move any remaining partial to the front
        size_t remaining = end - start;
        if (remaining > 0) memmove(p->partial, start, remaining);
        p->partial_len = (int)remaining;
        p->partial[p->partial_len] = 0;
    }

    // Process any final partial line after EOF
    if (p->partial_len > 0) {
        // treat as one final line
        char linebuf[4096];
        int linelen = p->partial_len;
        if (linelen >= (int)sizeof(linebuf)) linelen = (int)sizeof(linebuf) - 1;
        memcpy(linebuf, p->partial, linelen);
        linebuf[linelen] = 0;
        // convert to wide
        int wlen = MultiByteToWideChar(CP_UTF8, 0, linebuf, -1, NULL, 0);
        wchar_t *wline = NULL;
        if (wlen <= 0) {
            wlen = MultiByteToWideChar(CP_ACP, 0, linebuf, -1, NULL, 0);
            if (wlen > 0) {
                wline = (wchar_t*)malloc(wlen * sizeof(wchar_t));
                MultiByteToWideChar(CP_ACP, 0, linebuf, -1, wline, wlen);
            }
        } else {
            wline = (wchar_t*)malloc(wlen * sizeof(wchar_t));
            MultiByteToWideChar(CP_UTF8, 0, linebuf, -1, wline, wlen);
        }
        if (wline) {
            wchar_t *tline = TranslateConsoleLine(wline);
            free(wline);
            if (tline) {
                size_t tlen = wcslen(tline);
                wchar_t *wbuf = (wchar_t*)malloc((tlen + 3) * sizeof(wchar_t));
                if (wbuf) {
                    wcscpy(wbuf, tline);
                    wcscat(wbuf, L"\r\n");
                    PostMessageW(GetForegroundWindow(), WM_APPEND_OUTPUT, (WPARAM)p->hEdit, (LPARAM)wbuf);
                }
                free(tline);
            }
        }
        p->partial_len = 0;
        p->partial[0] = 0;
    }

    // Wait for process to exit
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hRead);
    free(p);
    return 0;
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
        AppendMenuW(hMenuIdioma, MF_STRING, 201, T(L"menu_es"));
        AppendMenuW(hMenuIdioma, MF_STRING, 202, T(L"menu_en"));
        AppendMenuW(hMenuTema, MF_STRING, 203, T(L"menu_dark"));
        AppendMenuW(hMenuTema, MF_STRING, 204, T(L"menu_light"));
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
        int dpiLocalLayout = GetWindowDPI(hwnd);
        int y = ScaleForWindow(hwnd, 15);
        int xlbl = ScaleForWindow(hwnd, 24);
        int xedit = ScaleForWindow(hwnd, 180);
        int xbtn = ScaleForWindow(hwnd, 520);
        int wlbl = ScaleForWindow(hwnd, 150);
        int wedit = ScaleForWindow(hwnd, 320);
        int wbtn = ScaleForWindow(hwnd, 32);
        int h = ScaleForWindow(hwnd, 26);
        int hBtn = ScaleForWindow(hwnd, 32);
        int hBrowse = ScaleForWindow(hwnd, 26);
        int sep = ScaleForWindow(hwnd, 12);
        int spcBeforeButtonsScaled = ScaleForWindow(hwnd, spcBeforeButtons);
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
        y += h + sep + ScaleForWindow(hwnd,10); // espacio extra entre PPF destino y file_id.diz

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
        
        // Ajustar ancho de botones al texto
        int btnMargin = ScaleForWindow(hwnd,20); // margen para bordes del botón
        int btnX = xlbl;
        
        wchar_t btnTxt[128];
        GetWindowTextW(hCrearBtnCrear, btnTxt, 128);
        int btnW = GetTextWidthInPixels(hCrearPanel, hFont, btnTxt) + btnMargin;
        MoveWindow(hCrearBtnCrear, btnX, y, btnW, hBtn, TRUE);
        btnX += btnW + ScaleForWindow(hwnd,10);
        
        GetWindowTextW(hCrearBtnShow, btnTxt, 128);
        btnW = GetTextWidthInPixels(hCrearPanel, hFont, btnTxt) + btnMargin;
        MoveWindow(hCrearBtnShow, btnX, y, btnW, hBtn, TRUE);
        btnX += btnW + ScaleForWindow(hwnd,10);
        
        GetWindowTextW(hCrearBtnAdd, btnTxt, 128);
        btnW = GetTextWidthInPixels(hCrearPanel, hFont, btnTxt) + btnMargin;
        MoveWindow(hCrearBtnAdd, btnX, y, btnW, hBtn, TRUE);
        btnX += btnW + ScaleForWindow(hwnd,10);
        
        GetWindowTextW(hCrearBtnClear, btnTxt, 128);
        btnW = GetTextWidthInPixels(hCrearPanel, hFont, btnTxt) + btnMargin;
        MoveWindow(hCrearBtnClear, btnX, y, btnW, hBtn, TRUE);
        y += hBtn + sep + spcBeforeButtonsScaled;

        // Label "Salida" removed — output edit will start here
        // y remains unchanged so hCrearOutput will be placed directly below the buttons.

        hCrearOutput = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            xlbl, y, ScaleForWindow(hwnd,570), ScaleForWindow(hwnd,120), hCrearPanel, (HMENU)140, NULL, NULL);
        g_hCrearOutput = hCrearOutput;

        // Command handlers: button IDs will be processed in WM_COMMAND

        // Panel solo visible en pestaña 0
        ShowWindow(hCrearPanel, SW_SHOW);

        // --- Crear panel y controles para 'Aplicar Parche' (pestaña 1)
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
        int btnMargin2 = 20;
        int btnX2 = xlbl;
        
        GetWindowTextW(hAplicarBtnApply, btnTxt2, 128);
        int btnW2 = GetTextWidthInPixels(hAplicarPanel, hFont, btnTxt2) + btnMargin2;
        MoveWindow(hAplicarBtnApply, btnX2, ay, btnW2, hBtn, TRUE);
        btnX2 += btnW2 + 10;
        
        GetWindowTextW(hAplicarBtnClear, btnTxt2, 128);
        btnW2 = GetTextWidthInPixels(hAplicarPanel, hFont, btnTxt2) + btnMargin2;
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
        for (int i = 0; i < sizeof(aplicarControles)/sizeof(HWND); ++i) {
            SendMessageW(aplicarControles[i], WM_SETFONT, (WPARAM)hFont, TRUE);
            ApplyTheme(aplicarControles[i]);
        }
        HWND crearControles[] = {hCrearLblImg, hCrearEditImg, hCrearBtnImg, hCrearLblMod, hCrearEditMod, hCrearBtnMod,
            hCrearLblPPF, hCrearEditPPF, hCrearBtnPPF, hCrearLblDIZ, hCrearEditDIZ, hCrearBtnDIZ, hCrearLblDesc, hCrearEditDesc,
            hCrearChkUndo, hCrearChkValid, hCrearLblTipo, hCrearComboTipo, hCrearBtnCrear, hCrearBtnShow, hCrearBtnAdd, hCrearBtnClear, hCrearOutput};
        for (int i = 0; i < sizeof(crearControles)/sizeof(HWND); ++i) {
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
        // Log control HWNDs for diagnostics
        {
            wchar_t dbg[512];
            _snwprintf(dbg, 512, tw("controls_dbg"), (void*)hCrearEditImg, (void*)hCrearBtnImg, (void*)hCrearEditMod, (void*)hCrearBtnMod, (void*)hCrearBtnPPF);
            LogToFile(dbg);
        }
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
            int wlbl_local = ScaleForWindow(hCrearPanel, 150);
            int gap = ScaleForWindow(hCrearPanel, 10);
            int wbtn_local = ScaleForWindow(hCrearPanel, 32);
            int h_browse = ScaleForWindow(hCrearPanel, 24);
            int xedit_local = xlbl_local + wlbl_local + ScaleForWindow(hCrearPanel, 10);
            int y_local = ScaleForWindow(hCrearPanel, 10);
            int h_local = ScaleForWindow(hCrearPanel, 24);
            int h_action = ScaleForWindow(hCrearPanel, 30); // taller buttons for primary actions

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
                y_local += h_local + gap + 10; // espacio extra
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
                int lbl_tipo_w = ScaleForWindow(hCrearPanel, 50);
                int small_gap = ScaleForWindow(hCrearPanel, 6);

                /* place checkboxes starting at left */
                MoveWindow(hCrearChkUndo, xlbl_local, y_local, chk_undo_w, h_local, TRUE);
                MoveWindow(hCrearChkValid, xlbl_local + chk_undo_w + chk_gap, y_local, chk_valid_w, h_local, TRUE);

                /* Position the label and combo using fixed (scaled) offsets so they align like the original layout */
                int lbl_left = xlbl_local + ScaleForWindow(hCrearPanel, 350);
                int combo_left = xlbl_local + ScaleForWindow(hCrearPanel, 405);
                MoveWindow(hCrearLblTipo, lbl_left, y_local + 3, ScaleForWindow(hCrearPanel,50), h_local, TRUE);
                MoveWindow(hCrearComboTipo, combo_left, y_local, wcombo, h_local * 6, TRUE);

                y_local += h_local + gap + spcBeforeButtons;
            }
            if (hCrearLblSalida) {
                MoveWindow(hCrearLblSalida, xlbl_local, y_local, ScaleForWindow(hCrearPanel,60), h_local, TRUE);
                y_local += h_local + 4;
            }
            if (hCrearBtnCrear && hCrearBtnShow && hCrearBtnAdd && hCrearBtnClear) {
                // Ajustar ancho de botones al texto
                int btnMargin = 20;
                int btnX = xlbl_local;
                wchar_t btnTxt[128];
                
                GetWindowTextW(hCrearBtnCrear, btnTxt, 128);
                int btnW = GetTextWidthInPixels(hCrearPanel, hFont, btnTxt) + btnMargin;
                MoveWindow(hCrearBtnCrear, btnX, y_local, btnW, h_action, TRUE);
                btnX += btnW + 10;
                
                GetWindowTextW(hCrearBtnShow, btnTxt, 128);
                btnW = GetTextWidthInPixels(hCrearPanel, hFont, btnTxt) + btnMargin;
                MoveWindow(hCrearBtnShow, btnX, y_local, btnW, h_action, TRUE);
                btnX += btnW + 10;
                
                GetWindowTextW(hCrearBtnAdd, btnTxt, 128);
                btnW = GetTextWidthInPixels(hCrearPanel, hFont, btnTxt) + btnMargin;
                MoveWindow(hCrearBtnAdd, btnX, y_local, btnW, h_action, TRUE);
                btnX += btnW + 10;
                
                GetWindowTextW(hCrearBtnClear, btnTxt, 128);
                btnW = GetTextWidthInPixels(hCrearPanel, hFont, btnTxt) + btnMargin;
                MoveWindow(hCrearBtnClear, btnX, y_local, btnW, h_action, TRUE);
                y_local += h_action + gap;
            }
            if (hCrearLblSalida) {
                MoveWindow(hCrearLblSalida, xlbl_local, y_local, 60, h_local, TRUE);
                y_local += h_local + 4;
            }
            if (hCrearOutput) {
                int outW = panelW - xlbl_local - marginRight;
                int outH = rcPanel.bottom - y_local - 10; // expandir hasta abajo con margen
                if (outH < 80) outH = 80;
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
                    if (aoutH < ScaleForWindow(hAplicarPanel,80)) aoutH = ScaleForWindow(hAplicarPanel,80);
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
        case 201: // Español
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
                        CheckMenuItem(hMenuIdioma, 201, MF_BYCOMMAND | MF_CHECKED);
                        CheckMenuItem(hMenuIdioma, 202, MF_BYCOMMAND | MF_UNCHECKED);
                    }
                    UpdateThemeMenuChecks(hMenuBar, g_isDark);
                }
                DrawMenuBar(g_hwndMain);
            }
            if (hCrearComboTipo) {
                SendMessageW(hCrearComboTipo, CB_RESETCONTENT, 0, 0);
                SendMessageW(hCrearComboTipo, CB_ADDSTRING, 0, (LPARAM)L"BIN");
                SendMessageW(hCrearComboTipo, CB_ADDSTRING, 0, (LPARAM)L"GI");
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
        case 202: // English
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
                        CheckMenuItem(hMenuIdioma, 201, MF_BYCOMMAND | MF_UNCHECKED);
                        CheckMenuItem(hMenuIdioma, 202, MF_BYCOMMAND | MF_CHECKED);
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
            // choose dialog type and filter
            if (id == 113) {
                // Save PPF (do not prompt on overwrite — auto-overwrite)
                ofn.lpstrFilter = L"PPF files\0*.ppf\0All files\0*.*\0\0";
                // initial dir from current edit if present
                BOOL hasInitialDir = FALSE;
                if (GetWindowTextW(hCrearEditPPF, initialDir, MAX_PATH) && wcslen(initialDir) > 0) {
                    GetParentFolder(initialDir, initialDir, MAX_PATH);
                    ofn.lpstrInitialDir = initialDir;
                    hasInitialDir = TRUE;
                } else {
                    // Use executable dir
                    static wchar_t exeDir[MAX_PATH] = {0};
                    GetExecutableDirectory(exeDir, MAX_PATH);
                    ofn.lpstrInitialDir = exeDir;
                }
                // Use COM dialog which actually respects initial directory
                if (ShowSaveFileDialog_COM(hwnd, filename, MAX_PATH, ofn.lpstrInitialDir, ofn.lpstrFilter)) {
                    if (hCrearEditPPF) SetWindowTextW(hCrearEditPPF, filename);
                }
            } else {
                ofn.Flags |= OFN_FILEMUSTEXIST;
                HWND hTarget = NULL;
                if (id == 111) hTarget = hCrearEditImg;
                else if (id == 112) hTarget = hCrearEditMod;
                else if (id == 114) hTarget = hCrearEditDIZ;
                // Set filter according to control: images -> BIN/GI, diz -> DIZ, fallback -> All files
                if (id == 111 || id == 112) {
                    ofn.lpstrFilter = L"BIN/GI files\0*.bin;*.gi\0All files\0*.*\0\0";
                } else if (id == 114) {
                    ofn.lpstrFilter = L"DIZ files\0*.diz\0All files\0*.*\0\0";
                } else {
                    ofn.lpstrFilter = L"All files\0*.*\0\0";
                }
                // initial dir from target if present
                BOOL hasInitialDir = FALSE;
                if (hTarget && GetWindowTextW(hTarget, initialDir, MAX_PATH) && wcslen(initialDir) > 0) {
                    GetParentFolder(initialDir, initialDir, MAX_PATH);
                    ofn.lpstrInitialDir = initialDir;
                    hasInitialDir = TRUE;
                } else {
                    // Use executable dir
                    static wchar_t exeDir2[MAX_PATH] = {0};
                    GetExecutableDirectory(exeDir2, MAX_PATH);
                    ofn.lpstrInitialDir = exeDir2;
                }
                // Use COM dialog which actually respects initial directory
                if (ShowOpenFileDialog_COM(hwnd, filename, MAX_PATH, ofn.lpstrInitialDir, ofn.lpstrFilter)) {
                    if (hTarget) SetWindowTextW(hTarget, filename);
                    // If user selected the original image in 'Crear', auto-fill PPF filename if empty
                    if (id == 111 && hCrearEditPPF) {
                        wchar_t curppf[MAX_PATH] = {0};
                        GetWindowTextW(hCrearEditPPF, curppf, MAX_PATH);
                        if (wcslen(curppf) == 0) {
                            // build ppf path by replacing extension with .ppf
                            wchar_t ppfpath[MAX_PATH];
                            wcscpy(ppfpath, filename);
                            wchar_t *dot = wcsrchr(ppfpath, L'.');
                            if (dot) *dot = 0; // remove extension
                            wcscat(ppfpath, L".ppf");
                            SetWindowTextW(hCrearEditPPF, ppfpath);
                        }
                    }
                } else {
                    DWORD cerr = CommDlgExtendedError();
                    if (cerr != 0) {
                        wchar_t msg[256];
                        _snwprintf(msg, 256, tw("getopen_failed"), id, cerr);
                        LogToFile(msg);
                    }
                }
            }
            break;
        }
        case 131: // Crear Parche (run MakePPF)
        {
            PROC_THREAD_PARAM *p = (PROC_THREAD_PARAM*)malloc(sizeof(PROC_THREAD_PARAM));
            p->hEdit = hCrearOutput;
            p->partial_len = 0; p->partial[0] = 0;
            // build command line from controls
            BuildCreateCmdLine(p->cmdline, sizeof(p->cmdline)/sizeof(wchar_t), hCrearEditImg, hCrearEditMod, hCrearEditPPF, hCrearEditDIZ, hCrearEditDesc, hCrearChkUndo, hCrearChkValid, hCrearComboTipo);
            // save current settings
            SaveSettings(hCrearEditImg, hCrearEditMod, hCrearEditPPF, hCrearEditDIZ, hCrearEditDesc, hCrearChkUndo, hCrearChkValid, hCrearComboTipo,
                         hAplicarEditImg, hAplicarEditPPF, hAplicarChkRevert);
                // Log the command line for diagnostics (log only)
            {
                wchar_t dbg[2048]; _snwprintf(dbg, 2048, tw("exec"), p->cmdline);
                LogToFile(dbg);
            }
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
                // Log the command (log only)
                {
                    wchar_t dbg[2048]; _snwprintf(dbg, 2048, tw("exec"), p->cmdline);
                    LogToFile(dbg);
                }
                HANDLE hThread = CreateThread(NULL, 0, ProcessCaptureThread, p, 0, NULL);
                if (hThread) CloseHandle(hThread); /* CRITICAL: Close thread handle to prevent leak */
            } else {
                AppendTextToEdit(hCrearOutput, tw("select_ppf_info"));
                free(p);
            }
            break;
        }
        case 133: // Añadir file_id.diz (a)
        {
            PROC_THREAD_PARAM *p = (PROC_THREAD_PARAM*)malloc(sizeof(PROC_THREAD_PARAM));
            p->hEdit = hCrearOutput;
            p->partial_len = 0; p->partial[0] = 0;
            p->cmdline[0] = 0;
            wcscpy(p->cmdline, L"MakePPF");
            wcscat(p->cmdline, L" a");
            wchar_t ppf[MAX_PATH]; wchar_t fileid[MAX_PATH];
            if (hCrearEditPPF) GetWindowTextW(hCrearEditPPF, ppf, MAX_PATH); else ppf[0]=0;
            if (hCrearEditDIZ) GetWindowTextW(hCrearEditDIZ, fileid, MAX_PATH); else fileid[0]=0;
            if (wcslen(ppf) && wcslen(fileid)) {
                AppendQuotedArg(p->cmdline, sizeof(p->cmdline)/sizeof(wchar_t), ppf);
                AppendQuotedArg(p->cmdline, sizeof(p->cmdline)/sizeof(wchar_t), fileid);
                // Log the command (log only)
                {
                    wchar_t dbg[2048]; _snwprintf(dbg, 2048, L"Ejecutar: %s", p->cmdline);
                    LogToFile(dbg);
                }
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
            // Set filter: image -> BIN/GI, ppf -> PPF
            if (id == 211) {
                ofn.lpstrFilter = L"BIN/GI files\0*.bin;*.gi\0All files\0*.*\0\0";
            } else {
                ofn.lpstrFilter = L"PPF files\0*.ppf\0All files\0*.*\0\0";
            }
            BOOL hasInitialDir = FALSE;
            if (hTarget && GetWindowTextW(hTarget, initialDir, MAX_PATH) && wcslen(initialDir) > 0) {
                GetParentFolder(initialDir, initialDir, MAX_PATH);
                ofn.lpstrInitialDir = initialDir;
                hasInitialDir = TRUE;
            } else {
                // Use executable dir
                static wchar_t exeDir3[MAX_PATH] = {0};
                GetExecutableDirectory(exeDir3, MAX_PATH);
                ofn.lpstrInitialDir = exeDir3;
            }
            // Use COM dialog which actually respects initial directory
            if (ShowOpenFileDialog_COM(hwnd, filename, MAX_PATH, ofn.lpstrInitialDir, ofn.lpstrFilter)) {
                if (hTarget) SetWindowTextW(hTarget, filename);
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
                        // Log only
                        {
                            wchar_t dbg[1024]; _snwprintf(dbg, 1024, L"Ejecutar: %s", p->cmdline); LogToFile(dbg);
                        }
                        HANDLE hThread = CreateThread(NULL, 0, ProcessCaptureThread, p, 0, NULL);
                        if (hThread) CloseHandle(hThread); /* CRITICAL: Close thread handle to prevent leak */
                    }
                }
            } else {
                DWORD cerr = CommDlgExtendedError();
                    if (cerr != 0) {
                    wchar_t msg[256];
                    _snwprintf(msg, 256, L"GetOpenFileNameW falló (apply id=%d) CommDlgExtendedError=0x%08lX", id, cerr);
                    LogToFile(msg);
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
            // save current settings
            SaveSettings(hCrearEditImg, hCrearEditMod, hCrearEditPPF, hCrearEditDIZ, hCrearEditDesc, hCrearChkUndo, hCrearChkValid, hCrearComboTipo,
                         hAplicarEditImg, hAplicarEditPPF, hAplicarChkRevert);
                // Log the command (do not display it in the UI)
                {
                    wchar_t dbg[2048]; _snwprintf(dbg, 2048, L"Ejecutar: %s", p->cmdline);
                    LogToFile(dbg);
                }
                HANDLE hThread = CreateThread(NULL, 0, ProcessCaptureThread, p, 0, NULL);
                if (hThread) CloseHandle(hThread); /* CRITICAL: Close thread handle to prevent leak */
            break;
        }
        case 232: // Limpiar salida aplicar
            SetWindowTextW(hAplicarOutput, L"");
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
    // --- MODO CONSOLA ---
    int argc = 0;
    LPWSTR *argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    BOOL attached_console = FALSE;
    HWND hCon = GetConsoleWindow();
    // Si ya tenemos consola (ejecutable de consola) o podemos adjuntarnos a la consola padre, proceder en modo CLI
    if (hCon || AttachConsole(ATTACH_PARENT_PROCESS)) {
        attached_console = TRUE;
        g_console_attached = 1;
    }

    if (attached_console) {
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

        // Establecer la locale a UTF-8 (ayuda con conversiones de banda estrecha)
        setlocale(LC_ALL, ".UTF-8");

        char **argv = (char **)malloc(argc * sizeof(char*));
        for (int i = 0; i < argc; ++i) {
            int len = WideCharToMultiByte(CP_UTF8, 0, argvW[i], -1, NULL, 0, NULL, NULL);
            argv[i] = (char*)malloc(len);
            WideCharToMultiByte(CP_UTF8, 0, argvW[i], -1, argv[i], len, NULL, NULL);
        }
        int handled = 0;
        if (argc > 1) {
            // --- MakePPF modo consola ---
            if (argv[1][0] == 'c' || argv[1][0] == 's' || argv[1][0] == 'a') {
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
                        printf("Usage: PPFManager.exe c [-sw ...] <original bin> <modified bin> <ppf>\n");
                        printf("  c : create PPF3.0 patch\n  a : add file_id.diz\n  s : show patch information\n");
                        printf("Switches:\n -u : include undo data\n -x : disable patchvalidation\n -i [0/1] : imagetype\n -d \"text\" : description\n -f \"file\" : file_id.diz\n");
                    } else {
                        CheckSwitches(argc, argv);
                        if (OpenFilesForCreate()) {
                            PPFCreatePatch();
                            printf("Patch created successfully.\n");
                        } else {
                            printf("Error: could not open files for patch creation.\n");
                        }
                        CloseAllFiles();
                    }
                } else if (argv[1][0] == 's') {
                    if (argc < 3) {
                        printf("Usage: PPFManager.exe s <ppf>\n");
                    } else {
                        /* Print header exactly as MakePPF does, before any other output. Prefix with a blank line so it starts on its own line in CMD for GUI-mode executables. */
                        printf("\r\nMakePPF v3.0 by =Icarus/Paradox= %s\n", __DATE__);
                        fflush(stdout);

                        ppf = _open(argv[2], _O_RDONLY | _O_BINARY);
                        if (ppf == -1) {
                            printf("Error: cannot open file '%s'\n", argv[2]);
                        } else if (!CheckIfPPF3()) {
                            printf("Error: file '%s' is no PPF3.0 patch\n", argv[2]);
                            _close(ppf);
                        } else {
                            PPFShowPatchInfo();
                            _close(ppf);
                            printf("Done.\n");
                        }
                    }
                } else if (argv[1][0] == 'a') {
                    if (argc < 4) {
                        printf("Usage: PPFManager.exe a <ppf> <file_id.diz>\n");
                    } else {
                        ppf = _open(argv[2], _O_BINARY | _O_RDWR);
                        fileid = _open(argv[3], _O_RDONLY | _O_BINARY);
                        if (ppf == -1 || fileid == -1) {
                            printf("Error: cannot open file(s)\n");
                        } else if (!CheckIfPPF3()) {
                            printf("Error: file '%s' is no PPF3.0 patch\n", argv[2]);
                        } else if (!CheckIfFileId()) {
                            PPFAddFileId();
                            printf("file_id.diz added successfully.\n");
                        } else {
                            printf("Error: patch already contains a file_id.diz\n");
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
                extern int ppf, bin;
                #define APPLY 1
                #define UNDO 2
                if (argc != 4) {
                    printf("Usage: PPFManager.exe <command> <binfile> <patchfile>\n");
                    printf("  a : apply PPF1/2/3 patch\n");
                    printf("  u : undo patch (PPF3 only)\n");
                } else {
                    if (OpenFiles(argv[2], argv[3])) {
                        printf("Error: could not open files.\n");
                    } else {
                        int x = PPFVersion(ppf);
                        if (argv[1][0] == 'a') {
                            if (x == 1) { ApplyPPF1Patch(ppf, bin); printf("PPF1 patch applied.\n"); }
                            else if (x == 2) { ApplyPPF2Patch(ppf, bin); printf("PPF2 patch applied.\n"); }
                            else if (x == 3) { ApplyPPF3Patch(ppf, bin, APPLY); printf("PPF3 patch applied.\n"); }
                            else { printf("Unknown patch version.\n"); }
                        } else if (argv[1][0] == 'u') {
                            if (x == 3) { ApplyPPF3Patch(ppf, bin, UNDO); printf("PPF3 patch undo applied.\n"); }
                            else { printf("Undo function is supported by PPF3.0 only\n"); }
                        }
                        _close(bin);
                        _close(ppf);
                    }
                }
                handled = 1;
            }
            if (!handled) {
                // Ayuda general
                printf("\nPPFManager - Uso en modo consola:\n");
                printf("  Crear parche:   PPFManager.exe c [opciones] <bin original> <bin modificado> <ppf>\n");
                printf("  Añadir fileid:  PPFManager.exe a <ppf> <file_id.diz>\n");
                printf("  Info parche:    PPFManager.exe s <ppf>\n");
                printf("  Aplicar parche: PPFManager.exe a <bin> <ppf>\n");
                printf("  Deshacer:       PPFManager.exe u <bin> <ppf>\n");
                printf("\n");
            }
        } else {
            // Sin argumentos pero en consola: mostrar ayuda y salir
            printf("\nPPFManager - Uso en modo consola:\n");
            printf("  Crear parche:   PPFManager.exe c [opciones] <bin original> <bin modificado> <ppf>\n");
            printf("  Añadir fileid:  PPFManager.exe a <ppf> <file_id.diz>\n");
            printf("  Info parche:    PPFManager.exe s <ppf>\n");
            printf("  Aplicar parche: PPFManager.exe a <bin> <ppf>\n");
            printf("  Deshacer:       PPFManager.exe u <bin> <ppf>\n");
            printf("\n");
        }
        for (int i = 0; i < argc; ++i) free(argv[i]);
        free(argv);
        // Forzar flush de stdout y stderr
        fflush(stdout);
        fflush(stderr);
        // Enviar un Enter al buffer de entrada de la consola para desbloquear el prompt
        HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
        INPUT_RECORD ir;
        DWORD written;
        ir.EventType = KEY_EVENT;
        ir.Event.KeyEvent.bKeyDown = TRUE;
        ir.Event.KeyEvent.wRepeatCount = 1;
        ir.Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
        ir.Event.KeyEvent.wVirtualScanCode = MapVirtualKey(VK_RETURN, MAPVK_VK_TO_VSC);
        ir.Event.KeyEvent.uChar.UnicodeChar = '\r';
        ir.Event.KeyEvent.dwControlKeyState = 0;
        WriteConsoleInput(hStdin, &ir, 1, &written);
        ir.Event.KeyEvent.bKeyDown = FALSE;
        WriteConsoleInput(hStdin, &ir, 1, &written);
        LocalFree(argvW);
        if (!hCon) FreeConsole();
        return 0;
    }
    /* Try to set process DPI awareness to Per Monitor v2 if supported (do this BEFORE creating any windows) */
    typedef BOOL (WINAPI *SetProcessDpiAwarenessContext_t)(void*);
    SetProcessDpiAwarenessContext_t pSetProcessDpiAwarenessContext = (SetProcessDpiAwarenessContext_t)GetProcAddress(GetModuleHandleW(L"user32"), "SetProcessDpiAwarenessContext");
    if (pSetProcessDpiAwarenessContext) {
        /* Per-monitor v2 context value is (DPI_AWARENESS_CONTEXT)-4 on supported OS */
        pSetProcessDpiAwarenessContext((void*)(INT_PTR)-4);
    }

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_STANDARD_CLASSES | ICC_TAB_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icex);

    /* Enable logging if environment variable PPFMANAGER_LOG or MAKEPPF_LOG is set to 1/true */
    wchar_t envbuf[32] = {0};
    if (GetEnvironmentVariableW(L"PPFMANAGER_LOG", envbuf, 32) > 0) {
        if (_wcsicmp(envbuf, L"1") == 0 || _wcsicmp(envbuf, L"true") == 0) ppfmanager_log_enabled = 1;
    } else if (GetEnvironmentVariableW(L"MAKEPPF_LOG", envbuf, 32) > 0) {
        if (_wcsicmp(envbuf, L"1") == 0 || _wcsicmp(envbuf, L"true") == 0) ppfmanager_log_enabled = 1;
    }

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    /* Load embedded icon resource (resource ID 101) using system DPI if available */
    int sysdpi = GetSystemDPI();
    int cxIcon = GetSystemMetricsForDpiSafe(SM_CXICON, sysdpi);
    int cyIcon = GetSystemMetricsForDpiSafe(SM_CYICON, sysdpi);
    int cxSm = GetSystemMetricsForDpiSafe(SM_CXSMICON, sysdpi);
    int cySm = GetSystemMetricsForDpiSafe(SM_CYSMICON, sysdpi);
    wc.hIcon = LoadIconWithScaleDownIfAvailable(hInstance, MAKEINTRESOURCEW(101), cxIcon, cyIcon);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    wc.lpszClassName = L"PPFManagerClass";
    /* Small icon (titlebar/taskbar) */
    wc.hIconSm = LoadIconWithScaleDownIfAvailable(hInstance, MAKEINTRESOURCEW(101), cxSm, cySm);
    RegisterClassExW(&wc);

    int base_w = 545, base_h = 670; // base size at 96 DPI

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
