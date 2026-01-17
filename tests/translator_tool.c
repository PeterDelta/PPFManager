#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// Simple single-pass replacer for ASCII-only rules used by tests.
struct R { const char *find; const char *repl; };

static char *translate_single_pass(const char *src, const struct R *rules, size_t n) {
    if (!src) return NULL;
    size_t srcLen = strlen(src);
    size_t outCap = srcLen + 64;
    char *out = (char*)malloc(outCap + 1);
    if (!out) return NULL;
    size_t outLen = 0;
    size_t pos = 0;
    while (pos < srcLen) {
        int matched = 0;
        for (size_t i = 0; i < n; ++i) {
            const char *find = rules[i].find;
            size_t fl = strlen(find);
            if (fl == 0) continue;
            if (pos + fl <= srcLen && strncmp(src + pos, find, fl) == 0) {
                const char *repl = rules[i].repl;
                size_t rl = strlen(repl);
                if (outLen + rl + 1 > outCap) {
                    while (outLen + rl + 1 > outCap) outCap *= 2;
                    char *nb = (char*)realloc(out, outCap + 1);
                    if (!nb) { free(out); return NULL; }
                    out = nb;
                }
                memcpy(out + outLen, repl, rl);
                outLen += rl;
                pos += fl;
                matched = 1;
                break;
            }
        }
        if (matched) continue;
        if (outLen + 2 > outCap) {
            outCap *= 2;
            char *nb = (char*)realloc(out, outCap + 1);
            if (!nb) { free(out); return NULL; }
            out = nb;
        }
        out[outLen++] = src[pos++];
    }
    out[outLen] = '\0';
    return out;
}

int main(void) {
    // Read all stdin into a buffer
    size_t cap = 4096; size_t len = 0;
    char *buf = (char*)malloc(cap + 1);
    if (!buf) return 1;
    int c;
    while ((c = getchar()) != EOF) {
        if (len + 2 > cap) {
            cap *= 2; char *nb = (char*)realloc(buf, cap + 1); if (!nb) { free(buf); return 1; } buf = nb;
        }
        buf[len++] = (char)c;
    }
    buf[len] = '\0';
    // Trim trailing newlines to simplify comparisons
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) { buf[--len] = '\0'; }

    const struct R rules[] = {
        { "entries found", "entradas encontradas" },
        { "entries", "entradas" },
        { "Done.", "Completado." }
    };
    char *out = translate_single_pass(buf, rules, sizeof(rules)/sizeof(rules[0]));
    if (!out) { free(buf); return 1; }
    printf("%s\n", out);
    free(out);
    free(buf);
    return 0;
}
