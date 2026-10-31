// Mod: frame-rate independent reinforcement timing.
// The original counts its delays in frames (10 frames between spawned actors,
// m_RepeatDelay frames between waves), and the Xbox game ran at 30 per second.
// At 60 fps waves came twice as fast. This counts in 30 Hz ticks instead;
// at a steady 30 fps it is one tick per frame, the original behaviour.
#include "XApp.h"
#include "XGameWnd.h"
extern XGameWnd *gpGameWnd[4];
extern "C" int cjmod_ticks30(unsigned int frame, float mul);

// MOD 0016de60
signed char XItemHandler_Reinforcements::DoUpdate()
{
    int t = cjmod_ticks30(gpGameWnd[0]->FrameCounter(), XGetFrameRateMul());
    if (m_RepeatTimer) {
        m_RepeatTimer -= t;
        if (m_RepeatTimer < 0) m_RepeatTimer = 0;
    }
    if (m_SpawnDelay) {
        m_SpawnDelay -= t;
        if (m_SpawnDelay < 0) m_SpawnDelay = 0;
    }
    if (m_bSpawning && m_SpawnDelay <= 0) {
        m_SpawnDelay = 10;
        Actor::Create(m_Profile, m_pItem->Position(), 0, 0.0f, 0, 0x100, 0);
        if (++m_Spawned >= m_Count) {
            m_bSpawning = 0;
            if (m_RepeatDelay) {
                m_RepeatTimer = m_RepeatDelay;
                return 1;
            }
            m_pItem->ItemFlags() = 0x10;
        }
    }
    return 1;
}
