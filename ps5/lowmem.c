/* malloc replacement that serves everything from a MAP_32BIT arena: the translated game stores
 * host pointers in 32-bit registers, so every heap object it can see must live below 2 GB. */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
#ifndef MAP_32BIT
#define MAP_32BIT 0
#endif
#ifdef NFS_PS5
/* The PS5 link routes every malloc-family call through --wrap (the platform heap in direct memory); these replace the wrappers
 * so that every allocation, the game's and the libraries', lands below 2 GB. */
#define W(n) __wrap_##n
#define ARENA (0x60000000u)   /* 1.5 GB of address space, touched pages only */
#else
#define W(n) n
#define ARENA (0x60000000u)   /* host test: the software Vulkan driver allocates from here too */
#endif
#define NCLS 48
typedef struct hdr { size_t cls; size_t link; } hdr;   /* 16 bytes, keeps 16-byte alignment; link = free-list next */
#define FREED ((size_t)1 << 40)
#define CLS(h) ((h)->cls & 0xff)
static uint8_t *arena, *top, *end;
static void *freel[NCLS];
static volatile int lk;
static unsigned long nfail;
static void lock(void) { while (__sync_lock_test_and_set(&lk, 1)) { } }
static void unlock(void) { __sync_lock_release(&lk); }
/* every mmap try is recorded so the launcher can log what the console accepted */
typedef struct { uintptr_t hint; size_t size; long result; int err; } attempt;
static attempt att[64]; static int natt;
static size_t asize;
static void *try_map(void *hint, size_t size, int flags) {
    void *p = mmap(hint, size, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (natt < 64) { att[natt].hint = (uintptr_t)hint; att[natt].size = size; att[natt].result = p == MAP_FAILED ? -1 : (long)(uintptr_t)p; att[natt].err = p == MAP_FAILED ? errno : 0; natt++; }
    return p;
}
int lowmem_attempt(int i, unsigned long *hint, unsigned long *size, long *result, int *err) {
    if (i < 0 || i >= natt) return 0;
    *hint = att[i].hint; *size = att[i].size; *result = att[i].result; *err = att[i].err; return 1;
}
static int init(void) {
    if (arena) return 1;
    static const size_t sizes[] = { ARENA, 0x40000000u, 0x30000000u, 0x20000000u, 0x10000000u, 0x08000000u };
    for (unsigned k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        size_t sz = sizes[k];
        int fl = MAP_PRIVATE | MAP_ANONYMOUS;
        void *p = try_map(0, sz, fl | MAP_NORESERVE | MAP_32BIT);
        if (p != MAP_FAILED && (uintptr_t)p + sz > 0x80000000u) { munmap(p, sz); p = MAP_FAILED; }
        if (p == MAP_FAILED) p = try_map(0, sz, fl | MAP_32BIT);
        if (p != MAP_FAILED && (uintptr_t)p + sz > 0x80000000u) { munmap(p, sz); p = MAP_FAILED; }
        /* MAP_32BIT is ignored by some systems and absent on others: probe explicit low addresses */
        for (uintptr_t hint = 0x10000000u; p == MAP_FAILED && hint + sz <= 0x80000000u && natt < 60; hint += 0x10000000u) {
            p = try_map((void *)hint, sz, fl | MAP_NORESERVE);
            if (p != MAP_FAILED && ((uintptr_t)p != hint || (uintptr_t)p + sz > 0x80000000u)) { munmap(p, sz); p = MAP_FAILED; }
        }
        if (p != MAP_FAILED) { arena = top = p; end = arena + sz; asize = sz; return 1; }
    }
    return 0;
}
static void *alloc(size_t n, size_t align) {
    size_t cls = 4; if (align < 16) align = 16;
    while (((size_t)1 << cls) < n + sizeof(hdr) + (align > 16 ? align : 0)) cls++;
    lock();
    if (!init()) { nfail++; unlock(); return 0; }
    void *r = freel[cls];
    if (r) { freel[cls] = (void *)((hdr *)r)->link; ((hdr *)r)->link = 0; ((hdr *)r)->cls &= ~FREED; }
    else {
        size_t sz = (size_t)1 << cls;
        if ((size_t)(end - top) < sz) { nfail++; unlock(); errno = ENOMEM; return 0; }
        r = top; top += sz; ((hdr *)r)->cls = cls;
    }
    unlock();
    uint8_t *u = (uint8_t *)((hdr *)r + 1);
    if (align > 16) { u = (uint8_t *)(((uintptr_t)u + align - 1) & ~(uintptr_t)(align - 1)); }
    return u;
}
/* aligned blocks: store the block start just before the user pointer via a marker header */
void *W(malloc)(size_t n) { return alloc(n, 16); }
void W(free)(void *p) {
    if (!p) return;
#ifdef LOWMEM_NOFREE
    return;
#endif
    if ((uint8_t *)p < arena || (uint8_t *)p >= end) return;   /* not ours (platform heap): leak it */
    hdr *h = (hdr *)p - 1; size_t cls = CLS(h);
    if ((uintptr_t)p & 15 || cls >= NCLS) return;   /* aligned (>16) blocks are leaked */
    /* the link lives in the header so a game write into freed memory cannot corrupt the list; double frees are ignored */
    lock();
    if (!(h->cls & FREED)) { h->cls |= FREED; h->link = (size_t)freel[cls]; freel[cls] = h; }
    unlock();
}
void *W(calloc)(size_t a, size_t b) { void *p = W(malloc)(a * b); if (p) memset(p, 0, a * b); return p; }
void *W(realloc)(void *p, size_t n) {
    if (!p) return W(malloc)(n);
    size_t old = ((size_t)1 << CLS((hdr *)p - 1)) - sizeof(hdr);
    if (n <= old) return p;
    void *q = W(malloc)(n); if (q) { memcpy(q, p, old); W(free)(p); } return q;
}
int W(posix_memalign)(void **o, size_t al, size_t n) { void *p = alloc(n, al); if (!p) return ENOMEM; *o = p; return 0; }
void *W(memalign)(size_t al, size_t n) { return alloc(n, al); }
void *W(aligned_alloc)(size_t al, size_t n) { return alloc(n, al); }
size_t W(malloc_usable_size)(void *p) { return p ? ((size_t)1 << CLS((hdr *)p - 1)) - sizeof(hdr) : 0; }

/* for the log: where the arena is and how much of it has been handed out */
void lowmem_stats(unsigned long *base, unsigned long *used, unsigned long *size) {
    (void)asize; if (!arena) init(); *base = (unsigned long)arena; *used = (unsigned long)(top - arena); *size = (unsigned long)(end - arena);
}

unsigned long lowmem_failures(void) { return nfail; }
