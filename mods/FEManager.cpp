// Mod: Quit Game on the title screen.
// On the "Please press START" page, Back/B (Esc on the keyboard) arms a quit: the prompt line reads
// "Press ~F again to quit the game" for 3 s, and a second press exits (src/native/hybrid/cjmod.c).
// The page's line shows "~E Start   ~F Quit Game" so the option is visible.
extern "C" char *cjmod_ui_block(void);
extern "C" void cjmod_ui_attach(char *p);
extern "C" char *cjmod_ui_str(int i);
extern "C" int cjmod_confirm(int id);
extern "C" int cjmod_confirm_state(int id);
extern "C" void cjmod_quit(void);

// MOD 000f5520
signed char Manager_PressStart(FEObject *pObj, FEObjEvent::Flags ev)
{
    FEManager *pMgr = FEManager::Instance(1);
    if (!cjmod_ui_block()) cjmod_ui_attach(new char[0x400]);
    switch (ev) {
    case 0x13:
        pMgr->SetPageState(0);
        pObj->Object(2)->m_Text = cjmod_ui_str(7);
        return 0;
    case 0x18:
        if (!pMgr->AttractMode() && pMgr->Object()->ChildSel() == pObj) {  // only while this page is showing
            if (FEListener::QueryButton(0x88000, (FEPad::Flags)0xf, 1)) {
                if (cjmod_confirm(0)) cjmod_quit();
                pObj->Object(2)->m_Text = cjmod_ui_str(2);
            } else if (cjmod_confirm_state(0) == 2) {
                pObj->Object(2)->m_Text = cjmod_ui_str(7);
            }
        }
        return 0;
    case 0x17:
        if (pMgr->PageState() == 0) {
            if (3.0f > pMgr->PageStateTimerSecs()) return 0;
            theApp.GameStateFlush((GameState::Flags)0x20, XApp::Frontend3DFile());
        }
        return 1;
    case 0x15:
        if (pMgr->PageState() == 1 && pMgr->PageStateTimerSecs() >= 5.0f) {
            pObj->Object(2)->SetMathmos((FEObjColour::Flags)2,
                                        FEMathmos(ex::RGBA(0x7f, 0x7f, 0x7f, 0x80), ex::RGBA(0x7f, 0x7f, 0x7f, 0x40), 90.0f,
                                                  (FEWaveform::Flags)1, 0),
                                        1);
            pMgr->SetPageState(1);
            return 1;
        }
        break;
    case 0x27:
        s_pPressStartObj->Script(0)->SetFrame(150.0f);
        s_pPressStartObj->Script(0)->m_State |= 0x80000;
        break;
    }
    return 0;
}
