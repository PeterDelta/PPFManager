#ifndef PPFMANAGER_H
#define PPFMANAGER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Shared utilities provided by PPFManager (available when building combined EXE).
   Individual tools can keep local implementations when built standalone by defining
   BUILD_STANDALONE. */

#ifndef BUILD_STANDALONE
/* Prototypes for shared helpers (available in combined build)
   Implementations live in PPFManager.c for the combined build. For standalone
   builds a copy of these implementations is provided below (centralized here
   to avoid duplication across modules). */
int safe_write(int fd, const void *buf, size_t count);
int safe_read(int fd, void *buf, size_t count);
int PromptYesNo(const char *prompt, int defaultYes);
void PrintWin32ErrorFmt(const char *fmt, ...);
void PrintDescriptionBytes(const unsigned char *desc);
void PrintRawTextBytes(const unsigned char *s);

/* Deterministic PPF helpers: explicit little-endian read/write helpers to avoid platform variance */
int write_le64(int fd, unsigned long long val);
int write_le16(int fd, unsigned short val);
int read_le64(int fd, unsigned long long *out);
int read_le16(int fd, unsigned short *out);
#endif /* BUILD_STANDALONE */

#if defined(BUILD_STANDALONE) && !defined(PPFMANAGER_IMPLEMENTATION)
/* Standalone inline implementations (centralized). These are static inline to
   avoid link collisions and to keep behavior identical across tools. */

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

#endif /* PPFMANAGER_H */
