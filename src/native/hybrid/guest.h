/*
 * guest.h -- Xbox-shaped object access for native C++ in the x64 hybrid.
 *
 * Guest objects stay in guest memory with their 32-bit layout. Native code
 * sees them through host references obtained from a guest VA; every pointer
 * FIELD is a 4-byte GuestPtr<T> holding a guest VA, never a host pointer.
 *
 * The single-source macros (GPTR / EX_VIRTUAL / EX_VPTR) are what a shared
 * pcj-decomp header would have to use so the same text compiles as raw
 * pointers/virtuals under VC7.1 (_M_IX86, the matching build) and as guest
 * views here. See docs/native-swap.md for what that does and does not cover.
 */
#pragma once
#include <cstdint>
#include <cstddef>
#include <type_traits>

#if defined(_M_IX86) && !defined(CJ_HYBRID)        /* VC7.1 matching build */
#  define GPTR(T)      T *
#  define EX_VIRTUAL   virtual
#  define EX_VPTR                                  /* compiler-generated */
#else                                              /* x64 hybrid */
#  define CJ_HYBRID 1
extern "C" ptrdiff_t g_xbox_mem_offset;

template <class T> inline T *guest_to_host(uint32_t va)
{
    return va ? reinterpret_cast<T *>((uintptr_t)va + g_xbox_mem_offset) : nullptr;
}
extern "C" void cjg_escape(const void *p);
/* Storing a host address into a guest pointer field would silently corrupt guest state:
 * anything outside the 4 GiB guest window aborts (cjg_escape). */
template <class T> inline uint32_t host_to_guest(const T *p)
{
    if (!p) return 0u;
    uintptr_t d = (uintptr_t)p - (uintptr_t)g_xbox_mem_offset;
    if (d >> 32) cjg_escape(p);
    return (uint32_t)d;
}

/* A 32-bit guest pointer with pointer syntax. Assigning a host T* stores its
 * guest VA (only valid for pointers INTO guest memory). */
template <class T> struct GuestPtr {
    uint32_t va;
    T *get() const { return guest_to_host<T>(va); }
    T *operator->() const { return get(); }
    T &operator*() const { return *get(); }
    T &operator[](ptrdiff_t i) const { return get()[i]; }
    explicit operator bool() const { return va != 0; }
    operator T *() const { return get(); }
    GuestPtr &operator=(T *p) { va = host_to_guest(p); return *this; }
    /* const T* source for a T* field (the view drops the pointee's const); still escape-checked */
    template <class U = T>
    typename std::enable_if<!std::is_const<U>::value, GuestPtr &>::type operator=(const T *p) { va = host_to_guest(p); return *this; }
    GuestPtr &operator=(decltype(nullptr)) { va = 0; return *this; }
    /* `p = 0`: the literal 0 is an exact int match, so it no longer ties between T* and nullptr_t.
     * Any other int would not compile as a pointer assignment in the original, so it traps. */
    GuestPtr &operator=(int v) { if (v) __builtin_trap(); va = 0; return *this; }
};
static_assert(sizeof(GuestPtr<int>) == 4, "GuestPtr must be 4 bytes");

#  define GPTR(T)      GuestPtr<T>
#  define EX_VIRTUAL                               /* no host vtables in guest objects */
#  define EX_VPTR      uint32_t __vfptr;           /* guest vtable VA, slot 0 */
#endif

/* Not implemented yet (see docs/native-swap.md): a virtual call from native
 * code would go through the GUEST vtable -- MEM32(this) -> MEM32(vt + 4*slot)
 * -> recomp_lookup_manual/recomp_lookup -> push args + a return VA on the
 * guest stack, set g_ecx, call, read g_eax. */
