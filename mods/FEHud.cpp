// Mod: boss health bar.
// The original shows a boss's bar only while the camera is locked on to it (and for 5 seconds
// after). With the modern controls a fight is often played without lock-on, so the bar came and
// went. A boss that loses health now registers too: its bar shows from the hit and stays while
// the fight goes on (each hit restarts the 5 seconds), whether or not it is locked on.
extern "C" float cjmod_boss_health(int slot, float h);

// MOD 000d2200
void FEHud::UpdateBossBar()
{
    for (int i = 0; i < 128; i++) {
        Actor *pBoss = g_aActorList[i].m_pActor;
        float h = -1.0f;
        if (pBoss && g_aActorList[i].m_bInWorld && pBoss->HasBossBar() && pBoss->GetProfile()->m_Honour > 0)
            h = pBoss->m_Health;
        float prev = cjmod_boss_health(i, h);
        if (h > 0.0f && prev > 0.0f && h < prev) RegisterBossHit(pBoss->m_RefID);
    }
    FEObject *pBar = m_pObject->Object(8);
    Actor *pActor = Actor::GetPtrFromID(XGetCamera(0)->m_TargetID);
    if (pActor && pActor->HasBossBar() && pActor->GetProfile()->m_Honour > 0 && XGetCamera(0)->m_Mode == 1)
        RegisterBossHit(pActor->m_RefID);
    else
        pActor = Actor::GetPtrFromID(m_BossHitID);
    if (pActor && pActor->GetProfile()->m_Honour > 0 && pActor->HasBossBar()) {
        if (!pBar) {
            pBar = m_pObject->ObjectAdd(8, new FEObject(FEObjType::OT_SCRIPT2, 0x1000025, 0x400038a, 0xfdffffff, FE_Blank, FE_Blank,
                                                        (EXW::TextAlign)4, 0, 0, 0, 0, (EXW::TextAlign)3));
            FE::_Init(pBar);
            FE::_Peek()->Script(0)->Play(0, 0x180000);
            FE::_Peek()->SetMathmos((FEObjColour::Flags)3, ex::RGBA(0x7f, 0x7f, 0x7f, 0x80), 1);
            FE::_Peek()->SetMathmos((FEObjColour::Flags)2,
                                    FEMathmos(ex::RGBA(0, 0, 0, 0), ex::RGBA(0x7f, 0x7f, 0x7f, 0x80), 64.0f, FEWaveform::WF_LINEAR, 1), 1);
            FE::_Peek()->Script(0)->TweakPosition(EXVector(0.0f, 0.0f, -1.0f, 0.0f));
            FE::_Pop();
        } else if (m_BossHitTimer == 5.0f) {
            ex::RGBA to(0x7f, 0x7f, 0x7f, 0x80);
            ex::RGBA from = pBar->Mathmos((FEObjColour::Flags)2)->m_Colour;
            pBar->SetMathmos((FEObjColour::Flags)2, FEMathmos(from, to, 64.0f, FEWaveform::WF_LINEAR, 1), 1);
        } else if (m_BossHitTimer < 1.0f) {
            pBar->SetMathmos((FEObjColour::Flags)2, ex::RGBA(0x7f, 0x7f, 0x7f, (u8)(int)(m_BossHitTimer * 127.0f)), 1);
        }
        pBar->Script(0)->SetAmountPlayed(pActor->m_Health / pActor->GetProfile()->m_Health);
    } else if (pBar) {
        pBar->m_DeleteNeeded = 1;
    }
    if (m_BossHitTimer > 0.0f) {
        m_BossHitTimer -= XApp::FrameLength();
        if (m_BossHitTimer <= 0.0f) m_BossHitID = 0;
    }
}
