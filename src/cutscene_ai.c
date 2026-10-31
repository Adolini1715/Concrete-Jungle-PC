/* Fix: enemies attacking the hidden player during cutscenes.
 *
 * In a cutscene the real player is hidden (XItemHandler_Player::Hide) and often
 * teleported to where play resumes, while a puppet acts the scene. Actors keep
 * running Actor::CheckSeePlayer on the hidden player, and those whose profile
 * reacts on sight (UpdateSeeingPlayer -> STATE_AttackPred) attack the empty
 * spot in front of the puppet: the hallway scene of Midsummer Night Massacre,
 * where the gangsters should run in and drop to their knees, does this.
 *
 * ActorOptionsManager::PostProcess (called from AIControl::Update) recomputes
 * ActorOptionsManager::m_bPlayerVisible every frame; with it 0,
 * CheckSeePlayer does nothing. After it runs, while a cutscene is on and the
 * player is hidden, this clears m_bPlayerVisible and every actor's
 * "player seen" state (m_StateFlags.m_bPlayerSeen, ActorFlags2 bit 2, the
 * seen timer), so nothing reacts to the hidden player. Off with
 * CJ_CUTSCENE_HIDE=0. Linker wrap: the caller is in another generated chunk.
 *
 * 2026-10-25: OFF by default (CJ_CUTSCENE_HIDE=1 turns it on). The hallway attack was not the
 * original's behaviour at all: the recompiler had cut AP_GetStateFromHash's jump table at 256 of
 * its 439 entries, so the profile's flag-channel reaction (channel 180 -> FS_PanicStand) resolved
 * to no state and the gangsters fell through to DefaultIdle and then AttackPred. With the full
 * table the reaction overrides the sighting Gosub in the same frame, exactly as on the Xbox. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "recomp_types.h"

extern void __real_m_PostProcess_ActorOptionsManager__SAXXZ_00022DE0(void);

void __wrap_m_PostProcess_ActorOptionsManager__SAXXZ_00022DE0(void)
{
    static int on = -1;
    uint32_t app = 0x00482718u, pl;
    __real_m_PostProcess_ActorOptionsManager__SAXXZ_00022DE0();
    if (on < 0) { const char *e = getenv("CJ_CUTSCENE_HIDE"); on = e && *e == '1'; }
    if (!on || !(MEM32(app + 0xE30) & 0xFF)) return;          /* theApp.m_bCutscene */
    pl = MEM32(0x0048FC98u);                                  /* XItemHandler_Player::m_pInstance */
    if (!pl || !(MEM32(pl + 0xD8C) & 0xFF)) return;           /* m_bHidden */
    MEM32(0x00394B4Cu) &= ~0x0000FF00u;                       /* ActorOptionsManager::m_bPlayerVisible (0x394B4D) = 0 */
    for (uint32_t i = 0; i < 128; i++) {                      /* actor base table: actor pointer at +0x10 */
        uint32_t a = MEM32(0x0047C3F0u + i * 0x18u + 0x10);
        if (!a) continue;
        MEM32(a + 0x93C) &= ~0x00200000u;                     /* m_StateFlags.m_bPlayerSeen */
        MEM32(a + 0x944) &= ~0x00000004u;                     /* ActorFlags2 bit 2: can see the player */
        MEM32(a + 0x814) = 0;                                 /* seen timer */
    }
}
