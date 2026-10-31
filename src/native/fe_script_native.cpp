/*
 * fe_script_native.cpp -- FEObjectScript::AddMarker (0x001077C0) built as
 * native x64 code operating on the guest (Xbox-shaped) object.
 *
 * The class block below is pcj-decomp include/FEObjectScript.h trimmed to the
 * fields this function needs and rewritten with the single-source macros of
 * hybrid/guest.h; static_asserts pin it to the Xbox layout. The method body
 * is pcj-decomp src/game/FEScript.cpp:117-124 verbatim.
 */
#include "hybrid/guest.h"

typedef signed char s8;
#define __int64 long long          /* VC7.1 spelling used throughout pcj-decomp */

class FEObject;
class XItem;
namespace EXW { enum TextAlign { TA_DUMMY }; }

// --- from pcj-decomp include/FEObjectScript.h (verbatim) ---
class FEScriptMarker {
public:
    FEScriptMarker() { Reset(); }
    void Reset()
    {
        m_Start = -1.0f;
        m_End = -1.0f;
        m_Unk8 = -1.0f;
        m_UnkC = -1.0f;
        m_Unk10 = -1;
        m_Event = 0;
    }
    float m_Start;         // 0x00 first frame
    float m_End;           // 0x04 last frame
    float m_Unk8;          // 0x08
    float m_UnkC;          // 0x0c
    int m_Unk10;           // 0x10
    unsigned int m_Event;  // 0x14 FEObjEvent that plays it
};

// --- from pcj-decomp include/EXList.h, pointer fields through GPTR ---
class EXDListItem {
public:
    GPTR(EXDListItem) m_pPrev;
    GPTR(EXDListItem) m_pNext;
};

// --- from pcj-decomp include/FEObjectScript.h, single-source form ---
// Hybrid: no C++ virtuals (the vptr is the guest's, held as a 32-bit VA).
// MSVC puts the vptr at 0 ahead of the non-polymorphic base; the hybrid has
// to spell that out, which it can only do by making the base a member.
class FEObjectScript
#ifndef CJ_HYBRID
    : public EXDListItem
#endif
{
public:
#ifdef CJ_HYBRID
    EX_VPTR                              // 0x000 guest vtable VA
    EXDListItem m_Base;                  // 0x004 (base subobject)
#else
    virtual ~FEObjectScript();
#endif
    FEObjectScript *AddMarker(FEScriptMarker &m);

    unsigned __int64 m_Id;               // 0x010
    signed char m_Refraction;            // 0x018
    GPTR(FEObject) m_pOwner;             // 0x01c
    GPTR(XItem) m_pItem;                 // 0x020
    GPTR(FEScriptMarker) m_pMarkers;     // 0x024 [16]
    int m_nMarkers;                      // 0x028
    // ... remaining fields unused by AddMarker
};

#ifdef CJ_HYBRID
/* x64 SysV aligns long long to 8, like MSVC x86 does inside structs. */
static_assert(offsetof(FEObjectScript, m_Id) == 0x10, "layout");
static_assert(offsetof(FEObjectScript, m_pOwner) == 0x1c, "layout");
static_assert(offsetof(FEObjectScript, m_pMarkers) == 0x24, "layout");
static_assert(offsetof(FEObjectScript, m_nMarkers) == 0x28, "layout");
static_assert(sizeof(FEScriptMarker) == 0x18, "layout");
#endif

// --- pcj-decomp src/game/FEScript.cpp:117 (verbatim) ---
FEObjectScript *FEObjectScript::AddMarker(FEScriptMarker &m)
{
    if (m_pMarkers && m_nMarkers < 16) {
        m_pMarkers[m_nMarkers] = m;
        m_nMarkers++;
    }
    return this;
}

/* Adapter-facing entry: guest VAs in, guest VA out. */
extern "C" uint32_t cj_native_FEObjectScript_AddMarker(uint32_t this_va, uint32_t marker_va)
{
    FEObjectScript *self = guest_to_host<FEObjectScript>(this_va);
    FEScriptMarker *m = guest_to_host<FEScriptMarker>(marker_va);
    return host_to_guest(self->AddMarker(*m));
}
