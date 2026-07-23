#include <stddef.h>
#include <string.h>
#include <stdarg.h>

/*
 * Bump allocator using a fixed DRAM region at 0x81000000.
 * This region is available and avoids BSS relocation overflow.
 */
#define HEAP_BASE ((unsigned char *)0x81000000UL)
#define HEAP_SIZE (4u * 1024u * 1024u)

static unsigned char *heap_ptr = (unsigned char *)0x81000000UL;

void bare_malloc_reset(void) { heap_ptr = HEAP_BASE; }

void *bare_malloc(size_t size) {
    if (size == 0) return (void *)heap_ptr;
    size = (size + 7u) & ~7u;
    void *p = (void *)heap_ptr;
    heap_ptr += size;
    return p;
}

void  bare_free(void *ptr)              { (void)ptr; }
void *bare_calloc(size_t n, size_t sz) { void *p = bare_malloc(n*sz); if(p) memset(p,0,n*sz); return p; }
void *bare_realloc(void *ptr, size_t sz){ (void)ptr; return bare_malloc(sz); }

/* ---- newlib stubs needed when -nostdlib + -lm ---- */

/* errno: math library calls __errno() to set error codes; not needed bare-metal */
static int _bare_errno = 0;
int *__errno(void) { return &_bare_errno; }

/* assert: newlib's assert() calls __assert_func; redirect to halt */
void __assert_func(const char *f, int l, const char *fn, const char *e) {
    (void)f; (void)l; (void)fn; (void)e;
    /* bare-metal: just halt — printf may be unsafe here */
    while (1) {}
}

/* ---- memmove ---- */
void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s || d >= s + n) { memcpy(d, s, n); }
    else { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}

/* ---- minimal sprintf ---- */
int bare_sprintf(char *buf, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char *out = buf;
    for (const char *f = fmt; *f; f++) {
        if (*f != '%') { *out++ = *f; continue; }
        f++;
        if (*f == 's') {
            const char *s = va_arg(args, const char *);
            if (!s) s = "(null)";
            while (*s) *out++ = *s++;
        } else if (*f == 'd' || *f == 'i') {
            int v = va_arg(args, int);
            if (v < 0) { *out++ = '-'; v = -v; }
            char tmp[12]; int n = 0;
            if (v == 0) tmp[n++] = '0';
            else { while (v) { tmp[n++] = (char)('0' + v%10); v /= 10; } }
            for (int k = n-1; k >= 0; k--) *out++ = tmp[k];
        } else if (*f == 'u') {
            unsigned v = va_arg(args, unsigned);
            char tmp[12]; int n = 0;
            if (v == 0) tmp[n++] = '0';
            else { while (v) { tmp[n++] = (char)('0' + v%10); v /= 10; } }
            for (int k = n-1; k >= 0; k--) *out++ = tmp[k];
        } else if (*f == 'c') { *out++ = (char)va_arg(args, int);
        } else if (*f == '%') { *out++ = '%';
        } else { *out++ = '%'; *out++ = *f; }
    }
    *out = '\0';
    va_end(args);
    return (int)(out - buf);
}
