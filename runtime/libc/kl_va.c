#include <string.h>
#include "kl_va.h"

// ---- readers over an AAPCS64 va_list ----
// GP regs occupy 8 bytes each in the save area; FP regs occupy 16 (they are q regs).
// Once the register save area is exhausted, arguments come from the stack, where
// AAPCS64 gives every variadic argument an 8-byte slot.
uint64_t kl_va_gp(kl_va *v) {
    if (v->gr_offs < 0) {
        uint64_t x = *(uint64_t *)((char *)v->gr_top + v->gr_offs);
        v->gr_offs += 8;
        return x;
    }
    uint64_t x = *(uint64_t *)v->stack;
    v->stack = (char *)v->stack + 8;
    return x;
}

double kl_va_fp(kl_va *v) {
    if (v->vr_offs < 0) {
        double d = *(double *)((char *)v->vr_top + v->vr_offs);
        v->vr_offs += 16;
        return d;
    }
    double d = *(double *)v->stack;
    v->stack = (char *)v->stack + 8;
    return d;
}

// ---- format walking ----
enum { CLS_NONE, CLS_INT32, CLS_INT64, CLS_DOUBLE };

// Darwin arm64 gives every VARIADIC argument its own 8-byte slot, regardless of
// the promoted type -- verified by tests/t_variadic.c, whose 9-int case reads with
// an 8-byte stride. (Natural-size packing applies to non-variadic stack arguments,
// not to the variadic area; assuming otherwise silently shifts every argument after
// the first `int`.) `long double` is 64-bit on Apple silicon, so %Lf is a double.
static void put(char *dst, size_t *off, size_t cap, const void *src,
                size_t size, size_t align, int *overflow) {
    size_t o = (*off + align - 1) & ~(align - 1);
    if (o + size > cap) { *overflow = 1; return; }
    memcpy(dst + o, src, size);
    *off = o + size;
}

size_t kl_va_marshal(const char *fmt, kl_va *src, void *dstv, size_t cap, int mode) {
    char *dst = dstv;
    size_t off = 0;
    int overflow = 0;

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') continue;
        p++;
        if (*p == '%') continue;

        if (mode == KL_VA_SCANF) {
            // '*' suppresses assignment -> no argument at all.
            int suppress = 0;
            if (*p == '*') { suppress = 1; p++; }
            while (*p >= '0' && *p <= '9') p++;                  // field width
            while (*p && strchr("hljztqL", *p)) p++;             // length modifier
            if (!*p) break;
            if (*p == '[') {                                     // scanset
                p++;
                if (*p == '^') p++;
                if (*p == ']') p++;
                while (*p && *p != ']') p++;
            }
            if (!suppress) { uint64_t v = kl_va_gp(src);
                             put(dst, &off, cap, &v, 8, 8, &overflow); }
            continue;
        }

        // flags
        while (*p && strchr("-+ #0'", *p)) p++;
        // width (or '*')
        if (*p == '*') { uint64_t w = kl_va_gp(src);
                         put(dst, &off, cap, &w, 8, 8, &overflow); p++; }
        else while (*p >= '0' && *p <= '9') p++;
        // precision
        if (*p == '.') {
            p++;
            if (*p == '*') { uint64_t pr = kl_va_gp(src);
                             put(dst, &off, cap, &pr, 8, 8, &overflow); p++; }
            else while (*p >= '0' && *p <= '9') p++;
        }
        // length modifier
        int lng = 0;                      // 0 = int-sized, 1 = 64-bit
        for (;;) {
            if (p[0] == 'h') { p++; if (*p == 'h') p++; }
            else if (p[0] == 'l') { lng = (p[1] == 'l') ? 1 : 1; p++; if (*p == 'l') p++; }
            else if (strchr("jztq", p[0]))  { lng = 1; p++; }
            else if (p[0] == 'L')           { p++; }   // long double == double here
            else break;
        }
        if (!*p) break;

        int cls;
        switch (*p) {
        case 'd': case 'i': case 'o': case 'u': case 'x': case 'X':
            cls = lng ? CLS_INT64 : CLS_INT32; break;
        case 'c':                          cls = CLS_INT32;  break;
        case 's': case 'p': case 'n':      cls = CLS_INT64;  break;
        case 'f': case 'F': case 'e': case 'E':
        case 'g': case 'G': case 'a': case 'A': cls = CLS_DOUBLE; break;
        default:                           cls = CLS_NONE;   break;
        }

        // The only distinction that matters is which register bank the value comes
        // from; every destination slot is 8 bytes either way.
        if (cls == CLS_INT32 || cls == CLS_INT64) {
            uint64_t v = kl_va_gp(src);
            put(dst, &off, cap, &v, 8, 8, &overflow);
        } else if (cls == CLS_DOUBLE) {
            double d = kl_va_fp(src);
            put(dst, &off, cap, &d, 8, 8, &overflow);
        }
    }
    return overflow ? (size_t)-1 : off;
}
