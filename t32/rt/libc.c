// Just enough C library for DOOM on TINY-32 (RV32IM, no operating system): strings, a heap, printf to the console
// port, and "files" that are memory (the WAD is part of the program image). Speed matters on a machine built from
// gates, so the hot ones (memcpy, memset) move a word at a time when they can.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>

#define CONSOLE (*(volatile uint32_t*)0xF0000000u)
#define EXIT (*(volatile uint32_t*)0xF0000010u)

int errno;

// ---------------------------------------------------------------- memory and strings
void* memcpy(void* d, const void* s, size_t n) {
    uint8_t* dp = d;
    const uint8_t* sp = s;
    if ((((uintptr_t)dp | (uintptr_t)sp) & 3) == 0) {
        for (; n >= 16; n -= 16, dp += 16, sp += 16) {
            uint32_t a = ((const uint32_t*)sp)[0], b = ((const uint32_t*)sp)[1], c = ((const uint32_t*)sp)[2], e = ((const uint32_t*)sp)[3];
            ((uint32_t*)dp)[0] = a, ((uint32_t*)dp)[1] = b, ((uint32_t*)dp)[2] = c, ((uint32_t*)dp)[3] = e;
        }
        for (; n >= 4; n -= 4, dp += 4, sp += 4) *(uint32_t*)dp = *(const uint32_t*)sp;
    }
    while (n--) *dp++ = *sp++;
    return d;
}

void* memmove(void* d, const void* s, size_t n) {
    uint8_t* dp = d;
    const uint8_t* sp = s;
    if (dp <= sp || dp >= sp + n) return memcpy(d, s, n);
    while (n--) dp[n] = sp[n];
    return d;
}

void* memset(void* d, int c, size_t n) {
    uint8_t* dp = d;
    uint32_t w = (uint8_t)c * 0x01010101u;
    while (n && ((uintptr_t)dp & 3)) *dp++ = (uint8_t)c, n--;
    for (; n >= 16; n -= 16, dp += 16) ((uint32_t*)dp)[0] = w, ((uint32_t*)dp)[1] = w, ((uint32_t*)dp)[2] = w, ((uint32_t*)dp)[3] = w;
    for (; n >= 4; n -= 4, dp += 4) *(uint32_t*)dp = w;
    while (n--) *dp++ = (uint8_t)c;
    return d;
}

int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q) return *p - *q;
    return 0;
}

void* memchr(const void* s, int c, size_t n) {
    const uint8_t* p = s;
    for (; n; n--, p++)
        if (*p == (uint8_t)c) return (void*)p;
    return 0;
}

size_t strlen(const char* s) { const char* p = s; while (*p) p++; return (size_t)(p - s); }
size_t strnlen(const char* s, size_t n) { size_t i = 0; while (i < n && s[i]) i++; return i; }
char* strcpy(char* d, const char* s) { char* r = d; while ((*d++ = *s++)) {} return r; }
char* strncpy(char* d, const char* s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
char* strcat(char* d, const char* s) { strcpy(d + strlen(d), s); return d; }
char* strncat(char* d, const char* s, size_t n) {
    char* e = d + strlen(d);
    while (n-- && *s) *e++ = *s++;
    *e = 0;
    return d;
}
int strcmp(const char* a, const char* b) { while (*a && *a == *b) a++, b++; return (uint8_t)*a - (uint8_t)*b; }
int strncmp(const char* a, const char* b, size_t n) {
    for (; n; n--, a++, b++)
        if (*a != *b || !*a) return (uint8_t)*a - (uint8_t)*b;
    return 0;
}
int strcasecmp(const char* a, const char* b) {
    while (*a && tolower((uint8_t)*a) == tolower((uint8_t)*b)) a++, b++;
    return tolower((uint8_t)*a) - tolower((uint8_t)*b);
}
int strncasecmp(const char* a, const char* b, size_t n) {
    for (; n; n--, a++, b++)
        if (tolower((uint8_t)*a) != tolower((uint8_t)*b) || !*a) return tolower((uint8_t)*a) - tolower((uint8_t)*b);
    return 0;
}
char* strchr(const char* s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char*)s;
        if (!*s) return 0;
    }
}
char* strrchr(const char* s, int c) {
    const char* r = 0;
    for (;; s++) {
        if (*s == (char)c) r = s;
        if (!*s) return (char*)r;
    }
}
char* strstr(const char* h, const char* n) {
    size_t k = strlen(n);
    for (; *h; h++)
        if (!strncmp(h, n, k)) return (char*)h;
    return k ? 0 : (char*)h;
}
char* strdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}
char* strerror(int e) { (void)e; return "error"; }

// ---------------------------------------------------------------- the heap: blocks with a size word, a free list
extern char _end[];
static char* brk_at = _end;
#define HEAP_TOP (0x02000000u - (512u << 10))   // (the stack keeps the top 512 KB)
typedef struct Block { size_t size; struct Block* next; } Block;
static Block* free_list;

void* malloc(size_t n) {
    n = (n + 7) & ~(size_t)7;
    for (Block **p = &free_list, *b; (b = *p); p = &b->next)
        if (b->size >= n) { *p = b->next; return (char*)b + 8; }
    char* at = (char*)(((uintptr_t)brk_at + 7) & ~(uintptr_t)7);
    if ((uintptr_t)at + 8 + n > HEAP_TOP) return 0;
    brk_at = at + 8 + n;
    ((Block*)at)->size = n;
    return at + 8;
}

void free(void* p) {
    if (!p) return;
    Block* b = (Block*)((char*)p - 8);
    b->next = free_list;
    free_list = b;
}

void* calloc(size_t n, size_t size) {
    void* p = malloc(n * size);
    if (p) memset(p, 0, n * size);
    return p;
}

void* realloc(void* p, size_t n) {
    if (!p) return malloc(n);
    size_t old = ((Block*)((char*)p - 8))->size;
    if (old >= n) return p;
    void* q = malloc(n);
    if (q) memcpy(q, p, old), free(p);
    return q;
}

// ---------------------------------------------------------------- numbers
long strtol(const char* s, char** end, int base) {
    while (isspace((uint8_t)*s)) s++;
    int neg = 0;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2, base = 16;
    else if (base == 0 && s[0] == '0') base = 8;
    else if (base == 0) base = 10;
    long v = 0;
    for (;; s++) {
        int d = isdigit((uint8_t)*s) ? *s - '0' : isalpha((uint8_t)*s) ? tolower((uint8_t)*s) - 'a' + 10 : 99;
        if (d >= base) break;
        v = v * base + d;
    }
    if (end) *end = (char*)s;
    return neg ? -v : v;
}
unsigned long strtoul(const char* s, char** end, int base) { return (unsigned long)strtol(s, end, base); }
int atoi(const char* s) { return (int)strtol(s, 0, 10); }
long atol(const char* s) { return strtol(s, 0, 10); }
double atof(const char* s) { return (double)strtol(s, 0, 10); }
int abs(int v) { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }
static uint32_t seed = 1;
int rand(void) { seed = seed * 1103515245u + 12345u; return (int)((seed >> 1) & RAND_MAX); }
void srand(unsigned s) { seed = s; }
char* getenv(const char* name) { (void)name; return 0; }
int system(const char* cmd) { (void)cmd; return -1; }

void qsort(void* base, size_t n, size_t size, int (*cmp)(const void*, const void*)) {
    char* a = base;
    char tmp[256];
    for (size_t i = 1; i < n; i++)
        for (size_t j = i; j > 0 && cmp(a + (j - 1) * size, a + j * size) > 0; j--) {
            memcpy(tmp, a + j * size, size);
            memcpy(a + j * size, a + (j - 1) * size, size);
            memcpy(a + (j - 1) * size, tmp, size);
        }
}

void exit(int code) {
    EXIT = (uint32_t)code;
    for (;;) ;
}
void abort(void) { exit(134); }

// ---------------------------------------------------------------- files: the WAD is memory; the console is a port
extern const uint8_t wad_start[], wad_end[];
struct FILE { const uint8_t* data; size_t size, pos; int console; };
static FILE console_file = {0, 0, 0, 1}, wad_file;
FILE* stdin = &console_file;
FILE* stdout = &console_file;
FILE* stderr = &console_file;

FILE* fopen(const char* path, const char* mode) {
    size_t n = strlen(path);
    if (mode[0] == 'r' && n >= 9 && !strcasecmp(path + n - 9, "doom1.wad")) {
        wad_file = (FILE){wad_start, (size_t)(wad_end - wad_start), 0, 0};
        return &wad_file;
    }
    errno = ENOENT;
    return 0;
}
int fclose(FILE* f) { (void)f; return 0; }
size_t fread(void* p, size_t size, size_t n, FILE* f) {
    if (!size || f->console) return 0;
    size_t left = (f->size - f->pos) / size;
    if (n > left) n = left;
    memcpy(p, f->data + f->pos, n * size);
    f->pos += n * size;
    return n;
}
size_t fwrite(const void* p, size_t size, size_t n, FILE* f) {
    if (!f->console) return 0;
    const char* s = p;
    for (size_t i = 0; i < size * n; i++) CONSOLE = (uint8_t)s[i];
    return n;
}
int fseek(FILE* f, long off, int whence) {
    long at = whence == SEEK_SET ? off : whence == SEEK_CUR ? (long)f->pos + off : (long)f->size + off;
    if (at < 0 || (size_t)at > f->size) return -1;
    f->pos = (size_t)at;
    return 0;
}
long ftell(FILE* f) { return (long)f->pos; }
int feof(FILE* f) { return f->pos >= f->size; }
int fflush(FILE* f) { (void)f; return 0; }
int fgetc(FILE* f) { return f->console || f->pos >= f->size ? EOF : f->data[f->pos++]; }
char* fgets(char* s, int n, FILE* f) {
    int i = 0;
    for (int c; i < n - 1 && (c = fgetc(f)) != EOF;) {
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    s[i] = 0;
    return i ? s : 0;
}
int fputc(int c, FILE* f) { if (f->console) CONSOLE = (uint8_t)c; return c; }
int fputs(const char* s, FILE* f) { while (*s) fputc(*s++, f); return 0; }
int putchar(int c) { CONSOLE = (uint8_t)c; return c; }
int puts(const char* s) { fputs(s, stdout); putchar('\n'); return 0; }
int remove(const char* path) { (void)path; return -1; }
int rename(const char* a, const char* b) { (void)a, (void)b; return -1; }
int access(const char* path, int mode) { (void)mode; FILE* f = fopen(path, "rb"); return f ? 0 : -1; }
int unlink(const char* path) { (void)path; return -1; }
int usleep(unsigned us) { (void)us; return 0; }
int open(const char* path, int flags, ...) { (void)path, (void)flags; return -1; }
int close(int fd) { (void)fd; return -1; }
int stat(const char* path, struct stat* st) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    st->st_size = (long)f->size, st->st_mode = 0100000;
    return 0;
}
int mkdir(const char* path, mode_t mode) { (void)path, (void)mode; return 0; }
int gettimeofday(struct timeval* tv, void* tz) {
    (void)tz;
    uint32_t ms = *(volatile uint32_t*)0xF0000004u;
    tv->tv_sec = ms / 1000, tv->tv_usec = (ms % 1000) * 1000;
    return 0;
}

// ---------------------------------------------------------------- printf (no floating point: DOOM prints none)
typedef struct { char* s; size_t n, at; } Out;
static void out(Out* o, char c) { if (o->at + 1 < o->n) o->s[o->at] = c; o->at++; }

int vsnprintf(char* s, size_t n, const char* fmt, va_list ap) {
    Out o = {s, n, 0};
    for (; *fmt; fmt++) {
        if (*fmt != '%') { out(&o, *fmt); continue; }
        fmt++;
        int left = 0, zero = 0, width = 0, prec = -1, longs = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') zero = 1;
            else if (*fmt != '+' && *fmt != ' ' && *fmt != '#') break;
        }
        if (*fmt == '*') width = va_arg(ap, int), fmt++;
        while (isdigit((uint8_t)*fmt)) width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            fmt++, prec = 0;
            if (*fmt == '*') prec = va_arg(ap, int), fmt++;
            while (isdigit((uint8_t)*fmt)) prec = prec * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') longs += *fmt++ == 'l';
        char buf[24];
        const char* str = buf;
        int len = 0, neg = 0;
        switch (*fmt) {
        case 'd': case 'i': {
            const int wide = longs > 1 || (longs == 1 && sizeof(long) == 8);   // (%ld: 64 bits on a 64-bit machine)
            long long v = wide ? va_arg(ap, long long) : va_arg(ap, int);
            unsigned long long u = v < 0 ? (neg = 1, -(unsigned long long)v) : (unsigned long long)v;
            do buf[sizeof buf - 1 - len++] = (char)('0' + u % 10); while (u /= 10);
            str = buf + sizeof buf - len;
            break;
        }
        case 'u': case 'x': case 'X': case 'p': {
            const int wide = longs > 1 || (longs == 1 && sizeof(long) == 8);
            unsigned long long u = *fmt == 'p' ? (uintptr_t)va_arg(ap, void*) : wide ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned);
            unsigned base = *fmt == 'u' ? 10 : 16;
            const char* dig = *fmt == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            do buf[sizeof buf - 1 - len++] = dig[u % base]; while (u /= base);
            str = buf + sizeof buf - len;
            break;
        }
        case 'c': buf[0] = (char)va_arg(ap, int), len = 1; break;
        case 's': str = va_arg(ap, const char*); if (!str) str = "(null)"; len = (int)strlen(str); if (prec >= 0 && len > prec) len = prec; break;
        case '%': buf[0] = '%', len = 1; break;
        case 'f': case 'g': case 'e': (void)va_arg(ap, double); buf[0] = '?', len = 1; break;
        default: buf[0] = *fmt, len = 1; break;
        }
        int zeros = (prec > len && *fmt != 's' && *fmt != 'c') ? prec - len : 0;   // (%.3d: at least 3 digits)
        int pad = width - len - neg - zeros;
        if (!left && !zero) while (pad-- > 0) out(&o, ' ');
        if (neg) out(&o, '-');
        if (!left && zero) while (pad-- > 0) out(&o, '0');
        while (zeros-- > 0) out(&o, '0');
        for (int i = 0; i < len; i++) out(&o, str[i]);
        if (left) while (pad-- > 0) out(&o, ' ');
    }
    if (n) s[o.at < n ? o.at : n - 1] = 0;
    return (int)o.at;
}

int vsprintf(char* s, const char* fmt, va_list ap) { return vsnprintf(s, 1u << 20, fmt, ap); }
int snprintf(char* s, size_t n, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsnprintf(s, n, fmt, ap); va_end(ap); return r; }
int sprintf(char* s, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsnprintf(s, 1u << 20, fmt, ap); va_end(ap); return r; }
int vfprintf(FILE* f, const char* fmt, va_list ap) {
    char buf[512];
    int r = vsnprintf(buf, sizeof buf, fmt, ap);
    fputs(buf, f);
    return r;
}
int vprintf(const char* fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }
int printf(const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfprintf(stdout, fmt, ap); va_end(ap); return r; }
int fprintf(FILE* f, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfprintf(f, fmt, ap); va_end(ap); return r; }

// sscanf: integers, hex, words and characters (what DOOM's config and argument parsing use)
int sscanf(const char* s, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int got = 0;
    for (; *fmt; fmt++) {
        if (isspace((uint8_t)*fmt)) { while (isspace((uint8_t)*s)) s++; continue; }
        if (*fmt != '%') { if (*s++ != *fmt) break; continue; }
        fmt++;
        int width = 0;
        while (isdigit((uint8_t)*fmt)) width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l' || *fmt == 'h') fmt++;
        char* end;
        if (*fmt == 'd' || *fmt == 'i' || *fmt == 'x' || *fmt == 'u') {
            long v = strtol(s, &end, *fmt == 'x' ? 16 : *fmt == 'i' ? 0 : 10);
            if (end == s) break;
            *va_arg(ap, int*) = (int)v, s = end, got++;
        } else if (*fmt == 's') {
            while (isspace((uint8_t)*s)) s++;
            char* d = va_arg(ap, char*);
            int k = 0;
            while (*s && !isspace((uint8_t)*s) && (!width || k < width)) d[k++] = *s++;
            d[k] = 0;
            if (!k) break;
            got++;
        } else if (*fmt == 'c') {
            if (!*s) break;
            *va_arg(ap, char*) = *s++, got++;
        } else break;
    }
    va_end(ap);
    return got;
}

// ---------------------------------------------------------------- the few maths functions DOOM links (never in a frame)
double fabs(double x) { return x < 0 ? -x : x; }
float fabsf(float x) { return x < 0 ? -x : x; }
