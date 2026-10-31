// Mod: pause menu Level page for the PC.
// Save Game and Load Game rows are added under Level Stats (the list grows from 4 rows to 6 the
// first frame). Choosing one shows a slot screen in the same list (a second set of six rows: a
// title and five slots for this chapter, swapped in by cjmod_pause_show); B / Back or the title
// goes back, a slot closes the menu and the save or load runs back in play (cjmod_pause_pick).
// The quick save (F5 / F8) is separate.
// Controller page: the Xbox pad model with labels at its 18 anchor points was cramped (labels
// overlapped). It becomes a flat modern pad picture (built-in texture, tools/art/controller_pause.py)
// with the labels in two columns of nine rows at its leader line ends, refreshed every frame so a
// Control Style change shows at once.
// The page's third row was the developers' "Quit Level" (it loads the 2D debug front end). It
// becomes "Quit Game" (press twice to confirm, like the main menu). The row is renamed in
// ContentUpdate, the first frame the page exists, so the 35 KB ContentCreate stays the original.
extern "C" char *cjmod_ui_block(void);
extern "C" void cjmod_ui_attach(char *p);
extern "C" char *cjmod_ui_str(int i);
extern "C" int cjmod_confirm(int id);
extern "C" int cjmod_confirm_state(int id);
extern "C" void cjmod_quit(void);
extern "C" int cjmod_pause_alive(void);
extern "C" int cjmod_pause_grow(char *list, char *mem);
extern "C" void cjmod_pause_keep(char *list, int set);
extern "C" void cjmod_pause_show(char *list, int set, int mode);
extern "C" void cjmod_pause_clear(char *list);
extern "C" int cjmod_pause_mode(void);
extern "C" void cjmod_pause_pick_attach(char *p);
extern "C" char *cjmod_pause_pick_label(int i);
extern "C" int cjmod_pause_pick(int slot);
extern "C" int cjmod_pause_qs_busy(void);
extern "C" void cjmod_ctrl_attach(char *p);
extern "C" unsigned int cjmod_ctrl_hash(int i);
extern "C" char *cjmod_ctrl_text(int i, const char *action);
extern "C" int cjmod_ctrl_kb(void);
extern "C" int cjmod_kb_row(int k);
extern "C" int cjmod_kb_col(int k);
extern "C" void cjmod_kb_cell(int k, char *obj);
extern "C" int cjmod_kb_focusable(int k);
extern "C" int cjmod_kb_relight(int k);
extern "C" char *cjmod_kb_text(int k, const char *action);

// MOD 000fb100
void FEPause::ContentUpdate()
{
    m_Mathmos3.Update();
    m_Mathmos.Update();
    m_Mathmos2.Update();
    if (m_pObject) {
        if (!cjmod_ui_block()) cjmod_ui_attach(new char[0x400]);
        int reopened = cjmod_pause_alive();
        // root -> pause screen (1) -> pages (2, 0) -> Level page (0) -> list (0, 0)
        FELayoutScrollGrid *pList = (FELayoutScrollGrid *)m_pObject->Object(1)->Object(2, 0)->Object(0)->Object(0, 0);
        FEListener *pNav = pList->Listener(0x5f5f5f5f5f76616eull);
        if (pList->m_ChildCount == 4 && cjmod_pause_grow((char *)pList, new char[6 * 4])) {
            // once: Level Stats, Save Game, Load Game, Restart Level, Quit Game, Quit to Main Menu
            for (int k = 0; k < 2; k++) {
                FEObject *p = pList->ObjectAdd(1 + k, 0, new FEObject((FEObjType::Flags)2, 0x100003a, 0x7000004, 0x51530000u + k,
                                                                     FE_Blank, FE_Blank, (EXW::TextAlign)4, 0, 0, 0, 0,
                                                                     (EXW::TextAlign)4));
                p->m_Text = cjmod_ui_str(8 + k);
            }
            cjmod_pause_keep((char *)pList, 0);
            // the slot screen: title + five slots, built into the list, kept, then the normal rows put back
            cjmod_pause_pick_attach(new char[6 * 64]);
            cjmod_pause_clear((char *)pList);   // ObjectAdd deletes what it replaces: keep the normal rows alive
            for (int k = 0; k < 6; k++)
                pList->ObjectAdd(k, 0, new FEObject((FEObjType::Flags)2, 0x100003a, 0x7000004, 0x51530010u + k, FE_Blank,
                                                    FE_Blank, (EXW::TextAlign)4, 0, 0, 0, 0, (EXW::TextAlign)4));
            cjmod_pause_keep((char *)pList, 1);
            cjmod_pause_show((char *)pList, 0, 0);
            pList->Resize();
            reopened = 0;
        }
        if (reopened && cjmod_pause_mode()) {   // the menu was closed on the slot screen
            cjmod_pause_show((char *)pList, 0, 0);
            pList->Resize();
            if (pNav) pNav->SetFocusByData(0x51530000u, 0);
        }
        {   // root -> pause screen (1) -> pages (2, 0) -> Controller page (4) -> body (0, 0) -> picture (1, 0)
            FEObject *pBody = m_pObject->Object(1)->Object(2, 0)->Object(4)->Object(0, 0);
            FEObject *pPic = pBody->Object(1, 0);
            if (pPic && pPic->m_Data == 0x5f63696870617267ull) {   // "graphic_": once per menu
                cjmod_ctrl_attach(new char[40 * 64]);
                FEObject *pLay = pBody->ObjectAdd(1, 0, new FELayoutLayered(3, 0, 0, 0x1d0, 0xda, (EXW::TextAlign)4));
                pLay->StateSub(8);
                pLay->ObjectAdd(0, new FEObject((FEObjType::Flags)1, 0x1000025, 0x86000076, 0xfdffffff, FE_Blank, FE_Blank,
                                                (EXW::TextAlign)4, 0, 0, 0x1d0, 0xda, (EXW::TextAlign)4));
                FELayoutScrollGrid *pSG = new FELayoutScrollGrid(9, 3, 0, 0, 0x1d0, 0xda, (EXW::TextAlign)4);
                float *pW = new float[3];   // label columns at the leader line ends (tools/art/controller_pause.py)
                pW[0] = 0.34f;
                pW[1] = 0.32f;
                pW[2] = 0.34f;
                pSG->SetGridWidthProportions(pW);
                pSG->SetVisibleCount(9, 3);
                pSG->StateSub(8);
                FEObject *pGrid = pLay->ObjectAdd(1, pSG);
                pGrid->m_Data = 0x5f7473696c6c7463ull;   // "ctrllist"
                for (int k = 0; k < 18; k++)
                    pGrid->ObjectAdd(k % 9, k < 9 ? 0 : 2,
                                     new FEObject((FEObjType::Flags)2, 0x100003a, 0x700000a, 0xfdffffff, FE_Blank, FE_Blank,
                                                  (EXW::TextAlign)(k < 9 ? 7 : 1), 0, 0, 0, 0, (EXW::TextAlign)4));
                // keyboard and mouse: an 11-row list (the actions with their keys, then the mouse settings),
                // its cells focusable for rebinding (src/femouse.c, src/keymap.c)
                FELayoutScrollGrid *pKG = new FELayoutScrollGrid(11, 3, 0, 0, 0x1d0, 0xda, (EXW::TextAlign)4);
                float *pW2 = new float[3];
                pW2[0] = 0.46f;
                pW2[1] = 0.08f;
                pW2[2] = 0.46f;
                pKG->SetGridWidthProportions(pW2);
                pKG->SetVisibleCount(11, 3);
                pKG->StateSub(8);
                FEObject *pKeys = pLay->ObjectAdd(2, pKG);
                for (int k = 0; k < 22; k++) {
                    FEObject *pCell = pKeys->ObjectAdd(cjmod_kb_row(k), cjmod_kb_col(k),
                                                       new FEObject((FEObjType::Flags)2, 0x100003a, 0x700000a, 0xfdffffff, FE_Blank,
                                                                    FE_Blank, (EXW::TextAlign)(cjmod_kb_col(k) == 0 ? 7 : 1), 0, 0, 0, 0,
                                                                    (EXW::TextAlign)4));
                    pCell->SetMathmos((FEObjColour::Flags)0,
                                      FEMathmos(ex::RGBA(0x7f, 0, 0, 0x80), ex::RGBA(0x7f, 0x7f, 0x7f, 0x80), 60.0f, FEWaveform::WF_LINEAR, 0), 1);
                    pCell->SetMathmos((FEObjColour::Flags)1, FEMathmos(ex::RGBA(0x7f, 0x7f, 0x7f, 0x80)), 1);
                }
                pBody->Resize();
            }
            FEObject *pLay2 = pBody->Object(1, 0);
            FEObject *pList2 = (pLay2 && pLay2->m_Data != 0x5f63696870617267ull) ? pLay2->Object(1) : 0;
            if (pList2 && pList2->m_Data == 0x5f7473696c6c7463ull) {
                FEObject *pKeys2 = pLay2->Object(2);
                ((XGamePad *)EXBaseGamePad::GetGamePad())->SetActions(GameData::Instance(1)->m_Values.m_ControllerConfig);
                for (int k = 0; k < 22; k++) {
                    EXString a = ((XGamePad *)EXBaseGamePad::GetGamePad())->GetControllerString(cjmod_ctrl_hash(k < 18 ? k : 0));
                    if (k < 18) {
                        FEObject *pCell = pList2->Object(k % 9, k < 9 ? 0 : 2);
                        if (pCell) pCell->m_Text = cjmod_ctrl_text(k, a.c_str());
                    }
                    FEObject *pKC = pKeys2 ? pKeys2->Object(cjmod_kb_row(k), cjmod_kb_col(k)) : 0;
                    if (pKC) {
                        pKC->m_Text = cjmod_kb_text(k, a.c_str());
                        cjmod_kb_cell(k, (char *)pKC);
                        if (cjmod_kb_focusable(k)) pKC->StateAdd(8);
                        else pKC->StateSub(8);
                        {
                            int lit = cjmod_kb_relight(k);
                            if (lit == 2) pKC->SetMathmos((FEObjColour::Flags)1, FEMathmos(ex::RGBA(0x7f, 0x10, 0x08, 0x80)), 1);
                            else if (lit == 1) pKC->SetMathmos((FEObjColour::Flags)1, FEMathmos(ex::RGBA(0x7f, 0x7f, 0x7f, 0x80)), 1);
                        }
                    }
                }
            }
        }
        FEObject *pFocus = pNav ? pNav->Focus() : 0;
        if (cjmod_pause_mode()) {
            for (int k = 0; k < 6; k++) {
                char *t = cjmod_pause_pick_label(k);
                if (t) pList->m_pChildren[k]->m_Text = t;
            }
            int back = FEListener::QueryButton(0x8000, (FEPad::Flags)0xf, 1) || FEListener::QueryButton(0x80000, (FEPad::Flags)0xf, 1);
            if (!back && pFocus && FEListener::QueryButton(0x42000, (FEPad::Flags)0xf, 1)) {
                int k = (int)((unsigned int)pFocus->m_Data - 0x51530010u);
                if (k == 0) back = 1;
                else if (k >= 1 && k <= 5)
                    EXBaseSoundManager::GetSoundManager()->StartSound(cjmod_pause_pick(k) ? 0x1af0015b : HT_Sfx_FE_Change);
            }
            if (back) {
                int load = cjmod_pause_mode() == 2;
                EXBaseSoundManager::GetSoundManager()->StartSound(0x1af00159);
                cjmod_pause_show((char *)pList, 0, 0);
                pList->Resize();
                if (pNav) pNav->SetFocusByData(0x51530000u + load, 0);
            }
        } else {
            for (unsigned int i = 0; i < pList->m_ChildCount; i++) {
                FEObject *pRow = pList->m_pChildren[i];
                if (!pRow) continue;
                unsigned int d = (unsigned int)pRow->m_Data;
                if (d == XApp::Frontend2DFile()) {   // the "Quit Level" row, once
                    pRow->m_Data = 0x51554954u;
                    pRow->m_Text = cjmod_ui_str(0);
                    d = 0x51554954u;
                }
                if (d == 0x51554954u) {
                    if (cjmod_confirm_state(2) == 2) pRow->m_Text = cjmod_ui_str(0);
                    if (pFocus == pRow && FEListener::QueryButton(0x42000, (FEPad::Flags)0xf, 1)) {
                        if (cjmod_confirm(2)) cjmod_quit();
                        pRow->m_Text = cjmod_ui_str(1);
                    }
                } else if ((d == 0x51530000u || d == 0x51530001u) && pFocus == pRow && !cjmod_pause_qs_busy() &&
                           FEListener::QueryButton(0x42000, (FEPad::Flags)0xf, 1)) {
                    EXBaseSoundManager::GetSoundManager()->StartSound(0x1af0015b);
                    cjmod_pause_show((char *)pList, 1, d == 0x51530000u ? 1 : 2);
                    pList->Resize();
                    if (pNav) pNav->SetFocusByData(0x51530011u, 0);
                    break;
                }
            }
        }
    }
    FEWnd::ContentUpdate();
}

// MOD 000fc300
signed char Pause_SelectMap(FEObject *pObj, FEObjEvent::Flags ev)
{
    switch (ev) {
    case 0x1a: {
        EXString t = gText.Get(0x51000838);
        if (!(t == pObj->m_Text)) {
            if ((unsigned int)pObj->m_Data == 0xabbaabbau) {
                EXBaseSoundManager::GetSoundManager()->StartSound(0x1af0015b);
                FEPause::Instance(1)->Object()->Object(1)->StateSub(0x10000);
                FEPause::Instance(1)->Object()->Object(2)->StateAdd(0x10040);
            } else if ((unsigned int)pObj->m_Data == 0x51554954u) {
                if (cjmod_confirm(2)) cjmod_quit();
                pObj->m_Text = cjmod_ui_str(1);
            } else {
                theApp.GameStatePush((GameState::Flags)0x1b, (unsigned int)pObj->m_Data);
                return 1;
            }
        }
        return 1;
    }
    case 0x13:
        pObj->Listener(0x5f5f5f5f5f76616eull)->RestoreDefaultFocus();
        return 1;
    default:
        return 0;
    }
}
