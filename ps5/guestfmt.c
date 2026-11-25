/* vsprintf for a 32-bit guest va_list (array of 4-byte slots; doubles/long long take two). */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
int guest_vsprintf(char *out, const char *fmt, const uint32_t *ap)
{
    char *o = out;
    while (*fmt) {
        if (*fmt != '%') { *o++ = *fmt++; continue; }
        char spec[64]; int n = 0; spec[n++] = *fmt++;
        if (*fmt == '%') { *o++ = '%'; fmt++; continue; }
        while (strchr("-+ #0", *fmt) && *fmt) spec[n++] = *fmt++;
        if (*fmt == '*') { n += sprintf(spec + n, "%d", (int)*ap++); fmt++; }
        else while (*fmt >= '0' && *fmt <= '9') spec[n++] = *fmt++;
        if (*fmt == '.') {
            spec[n++] = *fmt++;
            if (*fmt == '*') { n += sprintf(spec + n, "%d", (int)*ap++); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') spec[n++] = *fmt++;
        }
        int ll = 0, h = 0;
        for (;; fmt++) {
            if (*fmt == 'l') { if (fmt[1] == 'l') { ll = 1; fmt++; } }
            else if (*fmt == 'h') h++;
            else if (*fmt == 'L' || *fmt == 'j' || *fmt == 'q') ll = 1;
            else if (*fmt == 'z' || *fmt == 't') { }
            else break;
        }
        char c = *fmt ? *fmt++ : 0; if (!c) break;
        switch (c) {
        case 'd': case 'i':
            if (ll) { int64_t v; memcpy(&v, ap, 8); ap += 2; spec[n] = 0; strcat(spec, "lld"); o += sprintf(o, spec, (long long)v); }
            else { int v = (int)*ap++; if (h == 1) v = (short)v; else if (h > 1) v = (signed char)v; spec[n] = 0; strcat(spec, "d"); o += sprintf(o, spec, v); }
            break;
        case 'u': case 'x': case 'X': case 'o':
            if (ll) { uint64_t v; memcpy(&v, ap, 8); ap += 2; spec[n] = 0; strcat(spec, "ll"); n = strlen(spec); spec[n] = c; spec[n + 1] = 0; o += sprintf(o, spec, (unsigned long long)v); }
            else { unsigned v = *ap++; if (h == 1) v &= 0xFFFF; else if (h > 1) v &= 0xFF; spec[n] = c; spec[n + 1] = 0; o += sprintf(o, spec, v); }
            break;
        case 'c': spec[n] = 'c'; spec[n + 1] = 0; o += sprintf(o, spec, (int)*ap++); break;
        case 's': { const char *s = (const char *)(uintptr_t)*ap++; spec[n] = 's'; spec[n + 1] = 0; o += sprintf(o, spec, s ? s : "(null)"); break; }
        case 'p': { spec[n] = 'p'; spec[n + 1] = 0; o += sprintf(o, spec, (void *)(uintptr_t)*ap++); break; }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double d; memcpy(&d, ap, 8); ap += 2; spec[n] = c; spec[n + 1] = 0; o += sprintf(o, spec, d); break; }
        default: break;
        }
    }
    *o = 0; return (int)(o - out);
}
