/*
 * Fast emulated TLS for the MinGW GCC Windows build.
 *
 * MinGW GCC has no native TLS: every __thread access calls
 * __emutls_get_address. libgcc's version goes through winpthreads'
 * pthread_getspecific, which saves and restores GetLastError and takes a spin
 * lock on every call. The recompiled code reads and writes the guest registers
 * (g_eax .. g_esp, all __thread) constantly, so that path was most of the game
 * thread's own work.
 *
 * This replaces libgcc's emutls (same object layout, so code built by GCC calls
 * it unchanged; defining both entry points keeps libgcc_eh's emutls.o out of
 * the link). The per-thread array of variable pointers sits in a Windows TLS
 * slot; the fast path reads it straight from the TEB (TlsSlots at gs:0x1480,
 * for slot indices below 64; other slots take the slow path) and is a handful of instructions with no lock and
 * no call. First use of a variable on a thread takes the slow path.
 *
 * Variables are never freed when a thread exits (a few hundred bytes per
 * thread; the title creates a handful).
 */
#ifdef _WIN32
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

typedef struct {
    size_t size;
    size_t align;
    union { size_t offset; void *ptr; } loc;   /* 1-based index once assigned */
    void *templ;
} emutls_object;

typedef struct { size_t n; void *v[]; } emutls_array;   /* v[i - 1] is variable i */

static DWORD s_slot = TLS_OUT_OF_INDEXES;
static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;
static SRWLOCK s_lock = SRWLOCK_INIT;
static size_t s_count;

static BOOL CALLBACK init_slot(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    s_slot = TlsAlloc();
    return s_slot != TLS_OUT_OF_INDEXES;
}

__attribute__((noinline)) static void *slow_path(emutls_object *obj)
{
    DWORD err = GetLastError();   /* the guest-facing code keeps its own last error */
    size_t idx;
    emutls_array *a;
    void *p;

    InitOnceExecuteOnce(&s_once, init_slot, NULL, NULL);
    idx = __atomic_load_n(&obj->loc.offset, __ATOMIC_ACQUIRE);
    if (!idx) {
        AcquireSRWLockExclusive(&s_lock);
        idx = obj->loc.offset;
        if (!idx) {
            idx = ++s_count;
            __atomic_store_n(&obj->loc.offset, idx, __ATOMIC_RELEASE);
        }
        ReleaseSRWLockExclusive(&s_lock);
    }
    a = (emutls_array *)TlsGetValue(s_slot);
    if (!a || a->n < idx) {
        size_t n = idx + 32, old = a ? a->n : 0;
        emutls_array *b = (emutls_array *)realloc(a, sizeof(emutls_array) + n * sizeof(void *));
        if (!b) abort();
        memset(&b->v[old], 0, (n - old) * sizeof(void *));
        b->n = n;
        a = b;
        TlsSetValue(s_slot, a);
    }
    if (!(p = a->v[idx - 1])) {
        size_t al = obj->align > sizeof(void *) ? obj->align : sizeof(void *);
        p = _aligned_malloc(obj->size ? obj->size : 1, al);
        if (!p) abort();
        if (obj->templ) memcpy(p, obj->templ, obj->size);
        else memset(p, 0, obj->size);
        a->v[idx - 1] = p;
    }
    SetLastError(err);
    return p;
}

void *__emutls_get_address(emutls_object *obj)
{
    size_t idx = __atomic_load_n(&obj->loc.offset, __ATOMIC_ACQUIRE);
    DWORD slot = s_slot;
    if (__builtin_expect(idx != 0 && slot < 64, 1)) {
        emutls_array *a = (emutls_array *)__readgsqword(0x1480 + slot * 8);
        if (__builtin_expect(a && idx <= a->n, 1)) {
            void *p = a->v[idx - 1];
            if (__builtin_expect(p != NULL, 1))
                return p;
        }
    }
    return slow_path(obj);
}

void __emutls_register_common(emutls_object *obj, size_t size, size_t align, void *templ)
{
    if (obj->size < size) {
        obj->size = size;
        obj->templ = NULL;
    }
    if (obj->align < align)
        obj->align = align;
    if (templ && size == obj->size)
        obj->templ = templ;
}
#endif
