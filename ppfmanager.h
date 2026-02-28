#ifndef PPFMANAGER_H
#define PPFMANAGER_H

#include <stddef.h>
#include <stdio.h>

/* versión central */
#define PPF_VERSION_STR "1.02"

/* Las macros auxiliares permanecen en caso de que otro código desee realizar la conversión */
#define PPF_STR_HELPER(x) #x
#define PPF_STR(x) PPF_STR_HELPER(x)




#ifdef __cplusplus
extern "C" {
#endif

/* Utilidades compartidas proporcionadas por PPFManager (disponibles al compilar el EXE combinado).
   Las herramientas individuales pueden mantener implementaciones locales cuando se compilan de forma independiente definiendo
   BUILD_STANDALONE. */

/* Ayudantes de idioma y constantes disponibles tanto en compilaciones combinadas como independientes */

enum { LANG_ES = 0, LANG_EN = 1 };

#ifdef BUILD_STANDALONE
/* La variable de traducción independiente predeterminada es inglés */
static int g_lang = LANG_EN;
/* La versión independiente necesita su propia bandera de cierre; la GUI la establece en la compilación combinada */
volatile LONG g_app_closing = 0;
#else
extern int g_lang;
extern volatile LONG g_app_closing;
#endif

/* prototipos de ayudantes. al compilar de forma independiente se declaran static inline para coincidir
   con las implementaciones más adelante; de lo contrario son declaraciones extern normales */
#ifdef BUILD_STANDALONE
static inline const char *PPFManager_FileIdNameA(void);
static inline const wchar_t *PPFManager_FileIdNameW(void);
static inline const char *PPFManager_DescLabelA(void);
static inline const wchar_t *PPFManager_DescLabelW(void);
static inline const char *PPFManager_AddingA(void);
static inline const wchar_t *PPFManager_AddingW(void);
static inline int PPFManager_LabelWidth(void);
#else
const char *PPFManager_FileIdNameA(void);
const wchar_t *PPFManager_FileIdNameW(void);
const char *PPFManager_DescLabelA(void);
const wchar_t *PPFManager_DescLabelW(void);
const char *PPFManager_AddingA(void);
const wchar_t *PPFManager_AddingW(void);
int PPFManager_LabelWidth(void);
#endif

#ifndef BUILD_STANDALONE
/* Prototipos para ayudantes compartidos (disponibles en compilaciones combinadas). Las implementaciones se encuentran en PPFManager.c para la compilación combinada. Para compilaciones independientes, se proporciona a continuación una copia de estas implementaciones (centralizada aquí) para evitar la duplicación entre módulos.). */
int safe_write(int fd, const void *buf, size_t count);
int safe_read(int fd, void *buf, size_t count);
int PromptYesNo(const char *prompt, int defaultYes);
void PrintWin32ErrorFmt(const char *fmt, ...);
void PrintDescriptionBytes(const unsigned char *desc);
void PrintRawTextBytes(const unsigned char *s);

/* Cierre todos los archivos abiertos durante la creación/aplicación del parche.  */
void CloseAllFiles(void);

/* Ayudantes PPF deterministas: ayudantes de lectura/escritura little-endian explícitos para evitar la variación de la plataforma */
int write_le64(int fd, unsigned long long val);
int write_le16(int fd, unsigned short val);
int read_le64(int fd, unsigned long long *out);
int read_le16(int fd, unsigned short *out);
#endif /* BUILD_STANDALONE */

#if defined(BUILD_STANDALONE) && !defined(PPFMANAGER_IMPLEMENTATION)
/* Implementaciones en línea independientes (centralizadas). Estas son static inline para
   evitar colisiones de enlace y mantener el comportamiento idéntico entre herramientas. */

static inline int safe_write(int fd, const void *buf, size_t count) {
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

static inline int PromptYesNo(const char *prompt, int defaultYes) {
    int result = defaultYes ? 1 : 0;
    if (_isatty(_fileno(stdin))) {
        int c;
        printf("%s", prompt); fflush(stdout);
        c = getchar();
        if (c == EOF) return result;
        return (c == 'y' || c == 'Y');
    } else {
        char *env = getenv("PPFMANAGER_AUTO_YES");
        if (env && (_stricmp(env, "1") == 0 || _stricmp(env, "true") == 0)) return 1;
        return result;
    }
}

static inline wchar_t *ConvertToWidePreferUtf8ThenAcp(const char *s) {
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

static inline void PrintDescriptionBytes(const unsigned char *desc) {
    if (!desc) { int w = PPFManager_LabelWidth(); printf("%-*s : \n", w, PPFManager_DescLabelA()); return; }
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
                {
                    int width = PPFManager_LabelWidth();
                    wchar_t lab[64];
                    swprintf(lab, _countof(lab), L"%-*ls : ", width, PPFManager_DescLabelW());
                    WriteConsoleW(hOut, lab, (DWORD)wcslen(lab), &written, NULL);
                }
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
            if (!out) { free(w); int z = PPFManager_LabelWidth(); printf("%-*s : %s\n", z, PPFManager_DescLabelA(), to_print); return; }
            WideCharToMultiByte(CP_UTF8, 0, w, -1, out, need, NULL, NULL);
            { int z = PPFManager_LabelWidth(); printf("%-*s : %s\n", z, PPFManager_DescLabelA(), out); }
            if ((size_t)need > sizeof(outbuf)) free(out);
        } else {
            int z = PPFManager_LabelWidth(); printf("%-*s : %s\n", z, PPFManager_DescLabelA(), to_print);
        }
        free(w);
        return;
    }
    printf("Description : %s\n", to_print);
}

static inline void PrintRawTextBytes(const unsigned char *s) {
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

static inline void PrintWin32ErrorFmt(const char *fmt, ...) {
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

/* versiones independientes de los ayudantes de idioma para que PrintDescriptionBytes, etc. funcionen */
static inline const wchar_t *PPFManager_FileIdNameW(void) {
    return L"FileId";
}
static inline const char *PPFManager_FileIdNameA(void) {
    static char buf[64];
    WideCharToMultiByte(CP_UTF8, 0, PPFManager_FileIdNameW(), -1, buf, sizeof(buf), NULL, NULL);
    return buf;
}

static inline const wchar_t *PPFManager_DescLabelW(void) {
    return (g_lang == LANG_EN) ? L"Description" : L"Descripci\u00f3n";
}
static inline const char *PPFManager_DescLabelA(void) {
    static char buf[64];
    WideCharToMultiByte(CP_UTF8, 0, PPFManager_DescLabelW(), -1, buf, sizeof(buf), NULL, NULL);
    return buf;
}

static inline const wchar_t *PPFManager_AddingW(void) {
    return (g_lang == LANG_EN) ? L"Adding" : L"A\u00f1adiendo";
}
static inline const char *PPFManager_AddingA(void) {
    static char buf[64];
    WideCharToMultiByte(CP_UTF8, 0, PPFManager_AddingW(), -1, buf, sizeof(buf), NULL, NULL);
    return buf;
}

static inline int PPFManager_LabelWidth(void) {
    /* copiar la implementación de PPFManager.c para que las herramientas independientes se alineen correctamente */
    const wchar_t *d = PPFManager_DescLabelW();
    const wchar_t *f = PPFManager_FileIdNameW();
    size_t m = wcslen(d) > wcslen(f) ? wcslen(d) : wcslen(f);
    const wchar_t *others[] = {
        (g_lang == LANG_EN) ? L"Version" : L"Versi\u00f3n",
        (g_lang == LANG_EN) ? L"Enc.Method" : L"M\u00e9todo Enc.",
        (g_lang == LANG_EN) ? L"Imagetype" : L"Tipo de imagen",
        (g_lang == LANG_EN) ? L"Validation" : L"Validaci\u00f3n",
        (g_lang == LANG_EN) ? L"Undo Data" : L"Datos Deshacer",
    };
    for (size_t i = 0; i < sizeof(others)/sizeof(others[0]); ++i) {
        size_t len = wcslen(others[i]);
        if (len > m) m = len;
    }
    return (int)m;
}

static inline int write_le64(int fd, unsigned long long val) {
    unsigned char buf[8];
    for (int i = 0; i < 8; ++i) buf[i] = (unsigned char)((val >> (i*8)) & 0xFF);
    return safe_write(fd, buf, 8);
}
static inline int write_le16(int fd, unsigned short val) {
    unsigned char buf[2];
    buf[0] = (unsigned char)(val & 0xFF);
    buf[1] = (unsigned char)((val >> 8) & 0xFF);
    return safe_write(fd, buf, 2);
}

static inline int safe_read(int fd, void *buf, size_t count) {
    size_t read_bytes = 0;
    unsigned char *p = (unsigned char*)buf;
    while (read_bytes < count) {
        unsigned int chunk = (count - read_bytes) > (size_t)UINT_MAX ? UINT_MAX : (unsigned int)(count - read_bytes);
        int rv = _read(fd, p + read_bytes, chunk);
        if (rv < 0) return -1;
        if (rv == 0) return -1;
        read_bytes += (size_t)rv;
    }
    return 0;
}
static inline int read_le64(int fd, unsigned long long *out) {
    unsigned char buf[8];
    if (safe_read(fd, buf, 8) != 0) return -1;
    unsigned long long v = 0;
    for (int i = 0; i < 8; ++i) v |= ((unsigned long long)buf[i]) << (i*8);
    *out = v;
    return 0;
}
static inline int read_le16(int fd, unsigned short *out) {
    unsigned char buf[2];
    if (safe_read(fd, buf, 2) != 0) return -1;
    unsigned short v = (unsigned short)buf[0] | ((unsigned short)buf[1] << 8);
    *out = v;
    return 0;
}

#endif /* BUILD_STANDALONE */

#ifdef __cplusplus
}
#endif

/* Ayudantes de perfilado ligero opcionales habilitados en tiempo de ejecución mediante PPFMANAGER_PROFILE=1.
   Proporciona perf_enabled(), perf_now() y perf_report_ms(). Fallback multiplataforma usando clock(). */
#ifndef PPFMANAGER_PROFILE_HELPERS
#define PPFMANAGER_PROFILE_HELPERS
#include <time.h>
#ifdef _WIN32
#include <windows.h>
static inline int perf_enabled(void) {
    static int s = -1;
    if (s == -1) {
        char *e = getenv("PPFMANAGER_PROFILE");
        s = (e && (e[0] == '1')) ? 1 : 0;
    }
    return s;
}
static inline unsigned long long perf_now(void) {
    LARGE_INTEGER q; QueryPerformanceCounter(&q); return (unsigned long long)q.QuadPart;
}
static inline unsigned long long perf_freq(void) { static unsigned long long f = 0; if (!f) { LARGE_INTEGER q; QueryPerformanceFrequency(&q); f = (unsigned long long)q.QuadPart; } return f; }
static inline void perf_report_ms(const char *label, unsigned long long start, unsigned long long end) {
    if (!perf_enabled()) return;
    double ms = (double)(end - start) * 1000.0 / (double)perf_freq();
    fprintf(stderr, "[PERF] %s: %.3f ms\n", label, ms); fflush(stderr);
}
#else
static inline int perf_enabled(void) {
    static int s = -1;
    if (s == -1) { char *e = getenv("PPFMANAGER_PROFILE"); s = (e && (e[0] == '1')) ? 1 : 0; } return s;
}
static inline unsigned long long perf_now(void) { return (unsigned long long)clock(); }
static inline void perf_report_ms(const char *label, unsigned long long start, unsigned long long end) {
    if (!perf_enabled()) return;
    double ms = 1000.0 * (double)(end - start) / (double)CLOCKS_PER_SEC;
    fprintf(stderr, "[PERF] %s: %.3f ms\n", label, ms); fflush(stderr);
}
#endif
#endif /* PPFMANAGER_PROFILE_HELPERS */

#endif /* PPFMANAGER_H */
