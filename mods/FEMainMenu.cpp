// Mod: Options > PC Settings, and Quit Game on the main menu (UpdateStateMain: a fourth row,
// confirmed by a second press within 3 s).
//
// Options > Controller: the 3D Xbox pad with labels at its datums becomes the pause menu's flat
// modern pad picture with the labels in two columns (UpdateStateOptionsController).
//
// Options > PC Settings.
// The Options list gets a fifth row, "PC Settings". It opens the Audio page's
// state (0x10) with a flag set, and the Audio page then builds the PC page
// instead: the game's own checkboxes, spinners and sliders, bound to a block
// of guest memory that src/pcsettings.c watches, applies live and saves to
// pcj.ini. Strings and the column widths live in that block too (host data
// cannot be handed to guest code). Without the flag both pages are the
// originals, except that their string literals come from the block.
extern "C" char *cjmod_pc_block(void);
extern "C" void cjmod_pc_attach(char *p);
extern "C" int cjmod_pc_page(int set);
extern "C" int cjmod_pc_built(int set);
extern "C" int cjmod_pc_count(void);
extern "C" int cjmod_pc_kind(int i);
extern "C" int cjmod_pc_lo(int i);
extern "C" int cjmod_pc_hi(int i);
extern "C" int cjmod_pc_delta(int i);
extern "C" char *cjmod_pc_label(int i);
extern "C" char *cjmod_pc_help(int i);
extern "C" char *cjmod_pc_str(int i);
extern "C" int *cjmod_pc_val(int i);
extern "C" float *cjmod_pc_widths(void);
extern "C" char *cjmod_ui_block(void);
extern "C" void cjmod_ui_attach(char *p);
extern "C" char *cjmod_ui_str(int i);
extern "C" int cjmod_confirm(int id);
extern "C" int cjmod_confirm_state(int id);
extern "C" void cjmod_quit(void);
extern "C" void cjmod_ctrl_attach(char *p);
extern "C" unsigned int cjmod_ctrl_hash(int i);
extern "C" char *cjmod_ctrl_text(int i, const char *action);
extern "C" void cjmod_ctrl_page_alive(void);
extern "C" int cjmod_ctrl_kb(void);
extern "C" int cjmod_kb_row(int k);
extern "C" int cjmod_kb_col(int k);
extern "C" void cjmod_kb_cell(int k, char *obj);
extern "C" int cjmod_kb_focusable(int k);
extern "C" int cjmod_kb_relight(int k);
extern "C" char *cjmod_kb_text(int k, const char *action);
extern "C" char *cjmod_ctrl_grid(char *set);

// MOD 000e9a00
signed char FEMainMenu::UpdateStateOptions(FEObjEvent::Flags ev)
{
    FEObject *pGrid = s_OptionsGrid;
    switch (ev) {
    case 0x1e: {
        if (!cjmod_pc_block()) cjmod_pc_attach(new char[0x1000]);
        EXString sGame(cjmod_pc_str(1));
        EXString sPC(cjmod_pc_str(0));
        FEObject *p = m_pLayout->ObjectAdd(m_ListenerIdx, new FELayoutFixedGrid(5, 1, 0x5a, 0x9b, 0xb4, 0xc0, (EXW::TextAlign)0));
        s_OptionsGrid = p;
        s_OptionsGrid->ObjectAdd(0, 0, new FEObject(FEObjType::OT_TEXT, 0x100003a, 0x7000004, 0xe, Text_Get(0x510001bf), FE_Blank,
                                                    (EXW::TextAlign)4, 0, 0, 0, 0, (EXW::TextAlign)4));
        s_OptionsGrid->ObjectAdd(1, 0, new FEObject(FEObjType::OT_TEXT, 0x100003a, 0x7000004, 0xf, Text_Get(0x510001c0), FE_Blank,
                                                    (EXW::TextAlign)4, 0, 0, 0, 0, (EXW::TextAlign)4));
        s_OptionsGrid->ObjectAdd(2, 0, new FEObject(FEObjType::OT_TEXT, 0x100003a, 0x7000004, 0x10, Text_Get(0x510001c1), FE_Blank,
                                                    (EXW::TextAlign)4, 0, 0, 0, 0, (EXW::TextAlign)4));
        s_OptionsGrid->ObjectAdd(3, 0, new FEObject(FEObjType::OT_TEXT, 0x100003a, 0x7000004, 0x11, sGame, FE_Blank,
                                                    (EXW::TextAlign)4, 0, 0, 0, 0, (EXW::TextAlign)4));
        s_OptionsGrid->ObjectAdd(4, 0, new FEObject(FEObjType::OT_TEXT, 0x100003a, 0x7000004, 0x5043, sPC, FE_Blank,
                                                    (EXW::TextAlign)4, 0, 0, 0, 0, (EXW::TextAlign)4));
        s_OptionsGrid->ListenerAdd(new FEListener(0x5f5f5f5f5f76616e, 0x401, 0, (FEPad::Flags)0xf));
        FEListener *pl = s_OptionsGrid->Listener(0x5f5f5f5f5f76616e);
        m_UpdateListeners[m_ListenerIdx] = pl;
        s_OptionsGrid->Listener(0x5f5f5f5f5f76616e)->SetFocusByData(0xe, 0);
        s_OptionsGrid->SetMathmos((FEObjColour::Flags)0,
                                  FEMathmos(ex::RGBA(0x7f, 0, 0, 0x80), ex::RGBA(0x7f, 0x7f, 0x7f, 0x80), 60.0f, FEWaveform::WF_LINEAR, 0), 1);
        s_OptionsGrid->SetMathmos((FEObjColour::Flags)1, FEMathmos(ex::RGBA(0x7f, 0x7f, 0x7f, 0x80)), 1);
        break;
    }
    case 0x23:
        SetTitleAndBackgr(cjmod_pc_str(2), s_OptionsGrid->Rect());
        return 0;
    case 0x1f:
        m_b4 = 1;
        pGrid->Listener(0x5f5f5f5f5f76616e)->SetFocusByData(m_Unk81c, 0);
        return 0;
    case 0x17: {
        unsigned int d = (unsigned int)m_pCurLayout->Listener(0x5f5f5f5f5f76616e)->m_pFocus->m_Data;
        m_Unk81c = d;
        cjmod_pc_page(d == 0x5043);
        // the Audio state's page is built once and cached (bit 0x10 of m_Unk1fc);
        // when the other variant is wanted, drop the cache so UpdateState rebuilds it
        if ((d == 0x5043 || d == 0x10) && cjmod_pc_built(-1) != (d == 0x5043)) m_Unk1fc &= ~(1 << 0x10);
        GoState(d == 0x5043 ? 0x10 : d);
        return 1;
    }
    }
    return 0;
}

// MOD 000eaad0
signed char FEMainMenu::UpdateStateOptionsAudio(FEObjEvent::Flags ev)
{
    FEObject *pObj = 0;
    if (m_pCurLayout) pObj = m_pCurLayout->Object(1);
    switch (ev) {
    case 0x19:
        return 0;
    case 0x1e: {
        if (!cjmod_pc_block()) cjmod_pc_attach(new char[0x1000]);
        float *widths = cjmod_pc_widths();
        EXString sEmpty(cjmod_pc_str(3));
        s_OptionsAudioPage = m_pLayout->ObjectAdd(m_ListenerIdx, new FELayoutLayered(2, 0, 0, 0, 0, (EXW::TextAlign)4));
        cjmod_pc_built(cjmod_pc_page(-1));
        if (cjmod_pc_page(-1)) {
            int n = cjmod_pc_count();
            widths[0] = 0.62f;
            widths[1] = 0.38f;
            FELayoutScrollGrid *pGrid = new FELayoutScrollGrid(n, 2, 0x20, 0x8c, 0x15c, 0xf0, (EXW::TextAlign)0);
            s_OptionsAudioPage->ObjectAdd(1, pGrid);
            pGrid->SetGridWidthProportions(widths);
            pGrid->SetUpDownScrollArrowForceY(0);
            pGrid->SetVisibleCount(n < 9 ? n : 9, 2);   // scrolls: 13 rows at the larger menu text overlap (src/pcsettings.c order)
            for (int i = 0; i < n; i++) {
                int kind = cjmod_pc_kind(i);
                EXString sLabel(cjmod_pc_label(i));
                FEObject *pText = AddScrollGridText(pGrid, i, 0, 0x5043010000000000 + i, sLabel, (EXW::TextAlign)2,
                                                    sEmpty, kind != 0);
                FEObject *pCtl;
                if (kind == 0) {
                    pCtl = pGrid->ObjectAdd(i, 1, new FELayoutCheckbox(0, 0, 0, 0, 0, (EXW::TextAlign)4));
                    pCtl->m_Data = 0x5043000000000000 + i;
                    pCtl->Bind((FEDataType::Flags)1, cjmod_pc_val(i));
                } else if (kind == 1) {
                    pCtl = pGrid->ObjectAdd(i, 1, new FELayoutSlider(cjmod_pc_lo(i), cjmod_pc_hi(i), 0, 0, 0, 0, (EXW::TextAlign)4));
                    pCtl->m_Data = 0x5043000000000000 + i;
                    pCtl->SetRange(cjmod_pc_lo(i), cjmod_pc_hi(i));
                    pCtl->SetDelta(cjmod_pc_delta(i));
                    pCtl->StateModify(0, 0x8000000);
                    pCtl->Bind((FEDataType::Flags)3, cjmod_pc_val(i));
                } else {
                    pCtl = pGrid->ObjectAdd(i, 1, new FELayoutSpin(cjmod_pc_lo(i), cjmod_pc_hi(i), 0, 0, 0, 0, (EXW::TextAlign)4));
                    pCtl->m_Data = 0x5043000000000000 + i;
                    pCtl->SetRange(cjmod_pc_lo(i), cjmod_pc_hi(i));
                    pCtl->SetDelta(cjmod_pc_delta(i));
                    pCtl->StateModify(0, 0x8000000);
                    pCtl->Bind((FEDataType::Flags)3, cjmod_pc_val(i));
                }
                pCtl->SetHelpText(Text_Get(kind == 0 ? 0x510004d7 : 0x51000474));   // the stock footer hints (Toggle / Change)
                pText->MathmosLink(pCtl);
            }
            pGrid->ListenerAdd(new FEListener(0x5f5f5f5f5f76616e, 0xc03, 0, (FEPad::Flags)0xf));
            FEListener *pl = pGrid->Listener(0x5f5f5f5f5f76616e);
            m_UpdateListeners[m_ListenerIdx] = pl;
            pGrid->Listener(0x5f5f5f5f5f76616e)->SetFocus(0, 0);
            pGrid->SetMathmos((FEObjColour::Flags)0,
                              FEMathmos(ex::RGBA(0x7f, 0, 0, 0x80), ex::RGBA(0x7f, 0x7f, 0x7f, 0x80), 60.0f, FEWaveform::WF_LINEAR, 0), 1);
            pGrid->SetMathmos((FEObjColour::Flags)1, FEMathmos(ex::RGBA(0x7f, 0x7f, 0x7f, 0x80)), 1);
            return 0;
        }
        widths[0] = 0.55f;
        widths[1] = 0.45f;
        FELayoutScrollGrid *pGrid = new FELayoutScrollGrid(3, 2, 0x20, 0x9b, 0x15c, 0xdc, (EXW::TextAlign)0);
        s_OptionsAudioPage->ObjectAdd(1, pGrid);
        pGrid->SetGridWidthProportions(widths);
        pGrid->SetUpDownScrollArrowForceY(0);
        pGrid->SetVisibleCount(3, 2);
        FEObject *pText = AddScrollGridText(pGrid, 0, 0, 0x5f5f636973756d74, Text_Get(0x510001d7), (EXW::TextAlign)2, sEmpty, 1);
        FEObject *pSlider = pGrid->ObjectAdd(0, 1, new FELayoutSlider(0, 100, 0, 0, 0, 0, (EXW::TextAlign)4));
        pText->MathmosLink(pSlider);
        pSlider->m_Data = 0x5f5f5f636973756d;
        pSlider->SetRange(0, 100);
        pSlider->SetDelta(5);
        pSlider->StateModify(0, 0x8000000);
        pSlider->SetHelpText(Text_Get(0x51000474));
        pSlider->Bind((FEDataType::Flags)3, &GameData::Instance(1)->m_Values.m_MusicVolume);
        pSlider->SetEventCallback(Pause_ChangeVolume, 1);
        pText = AddScrollGridText(pGrid, 1, 0, 0x5f5f5f5f78667374, Text_Get(0x510001d8), (EXW::TextAlign)2, sEmpty, 1);
        pSlider = pGrid->ObjectAdd(1, 1, new FELayoutSlider(0, 100, 0, 0, 0, 0, (EXW::TextAlign)4));
        pText->MathmosLink(pSlider);
        pSlider->m_Data = 0x5f5f5f5f5f786673;
        pSlider->SetRange(0, 100);
        pSlider->SetDelta(5);
        pSlider->StateModify(0, 0x8000000);
        pSlider->SetHelpText(Text_Get(0x51000474));
        pSlider->Bind((FEDataType::Flags)3, &GameData::Instance(1)->m_Values.m_MovieVolume);
        pSlider->SetEventCallback(Pause_ChangeVolume, 1);
        pText = AddScrollGridText(pGrid, 2, 0, 0x5f5f5f7462755374, Text_Get(0x510001de), (EXW::TextAlign)2, sEmpty, 0);
        FEObject *pCheck = pGrid->ObjectAdd(2, 1, new FELayoutCheckbox(0, 0, 0, 0, 0, (EXW::TextAlign)4));
        pCheck->m_Data = 0x5f5f5f5f74627553;
        pCheck->Bind((FEDataType::Flags)1, &GameData::Instance(1)->m_Values.m_Unk1c[2]);
        pCheck->SetHelpText(Text_Get(0x510004d7));
        pText->MathmosLink(pCheck);
        pGrid->ListenerAdd(new FEListener(0x5f5f5f5f5f76616e, 0xc03, 0, (FEPad::Flags)0xf));
        FEListener *pl = pGrid->Listener(0x5f5f5f5f5f76616e);
        m_UpdateListeners[m_ListenerIdx] = pl;
        pGrid->Listener(0x5f5f5f5f5f76616e)->SetFocus(0, 0);
        pGrid->SetMathmos((FEObjColour::Flags)0,
                          FEMathmos(ex::RGBA(0x7f, 0, 0, 0x80), ex::RGBA(0x7f, 0x7f, 0x7f, 0x80), 60.0f, FEWaveform::WF_LINEAR, 0), 1);
        pGrid->SetMathmos((FEObjColour::Flags)1, FEMathmos(ex::RGBA(0x7f, 0x7f, 0x7f, 0x80)), 1);
        return 0;
    }
    case 0x1f:
        if (!pObj->Listener(0x5f5f5f5f5f76616e)->m_pFocus) {
            pObj->Listener(0x5f5f5f5f5f76616e)->SetFocusByData(cjmod_pc_page(-1) ? 0x5043000000000000 : 0x5f5f5f636973756d, 0);
            return 0;
        }
        break;
    case 0x23:
        SetTitleAndBackgr(cjmod_pc_str(cjmod_pc_page(-1) ? 0 : 2), pObj->Rect());
        return 0;
    case 0x21:
        pObj->Listener(0x5f5f5f5f5f76616e)->SetFocus(0, 0);
        break;
    }
    return 0;
}

// MOD 000e7960
signed char FEMainMenu::UpdateStateMain(FEObjEvent::Flags ev)
{
    switch (ev) {
    case 0x1e: {
        if (!cjmod_ui_block()) cjmod_ui_attach(new char[0x400]);
        FEObject *pPage = m_pLayout->ObjectAdd(m_ListenerIdx, new FELayoutLayered(2, 0x5a, 0xb1, 0xb4, 0xb3, (EXW::TextAlign)0));
        pPage->m_Flags &= ~8;
        s_MainGrid = pPage->ObjectAdd(1, new FELayoutFixedGrid(4, 1, 0, 0, 0xb4, 0xb3, (EXW::TextAlign)0));
        s_MainGrid->ObjectAdd(0, 0, new FEObject(FEObjType::OT_TEXT, 0x100003a, 0x7000004, 2, Text_Get(0x510001b8), FE_Blank,
                                                 (EXW::TextAlign)4, 0, 0, 0, 0, (EXW::TextAlign)4));
        s_MainButtonLoad = s_MainGrid->ObjectAdd(1, 0, new FEObject(FEObjType::OT_TEXT, 0x100003a, 0x7000004, 3, Text_Get(0x510001b9), FE_Blank,
                                                                    (EXW::TextAlign)4, 0, 0, 0, 0, (EXW::TextAlign)4));
        s_MainGrid->ObjectAdd(2, 0, new FEObject(FEObjType::OT_TEXT, 0x100003a, 0x7000004, 0xd, Text_Get(0x510001ba), FE_Blank,
                                                 (EXW::TextAlign)4, 0, 0, 0, 0, (EXW::TextAlign)4));
        // PC: Quit Game (data 'QUIT'), confirmed by a second press.
        s_MainGrid->ObjectAdd(3, 0, new FEObject(FEObjType::OT_TEXT, 0x100003a, 0x7000004, 0x51554954, FE_Blank, FE_Blank,
                                                 (EXW::TextAlign)4, 0, 0, 0, 0, (EXW::TextAlign)4))
            ->m_Text = cjmod_ui_str(0);
        s_MainGrid->ListenerAdd(new FEListener(0x5f5f5f5f5f76616eull, 0x401, 0, (FEPad::Flags)0xf));
        FEListener *pNav = s_MainGrid->Listener(0x5f5f5f5f5f76616eull);
        SetUpdateListener(pNav);
        s_MainGrid->Listener(0x5f5f5f5f5f76616eull)->SetFocusByData(2, 0);
        s_MainGrid->SetMathmos((FEObjColour::Flags)0,
                               FEMathmos(ex::RGBA(0x7f, 0, 0, 0x80), ex::RGBA(0x7f, 0x7f, 0x7f, 0x80), 60.0f, (FEWaveform::Flags)1, 0), 1);
        s_MainGrid->SetMathmos((FEObjColour::Flags)1, FEMathmos(ex::RGBA(0x7f, 0x7f, 0x7f, 0x80)), 1);
        return 1;
    }
    case 0x23:
        SetTitleAndBackgr("", s_MainGrid->Rect());
        ms_bDisplayStats = 0;
        ms_InitialLoadedFocus = 9;
        GameData::Instance(1)->m_Values.m_Unkc = 0x1000136;
        GameData::Instance(1)->Stats()->Reset();
        GameData::Instance(1)->m_Values.m_Unk1c[0] = 1;
        for (unsigned int off = 0; off < 0xaf8; off += 0x68)
            ((GameStats *)((char *)GameData::Instance(1)->m_Values.m_LevelStats + off))->Reset();
        GameData::Instance(1)->GetBigSave()->ResetRitualTimes();
        GameData::Instance(1)->GetRitualTimes()->Reset();
        GameData::Instance(1)->m_Values.m_Unk1c[4] = 0;
        GameData::Instance(1)->m_Values.m_Unk1c[5] = 0;
        GameData::Instance(1)->GetBigSave()->SetStatusAndReset(0);
        FEListener::SetPadOverride((FEPad::Flags)0xf);
        GameData::Instance(1)->Values()->ResetToDefaults(1);
        if (m_PlayerSkin != 0) {
            m_PlayerSkin = 0;
            m_PredSkinUpdateState = 0;
            m_PredSkinUpdateTime = 0.0f;
            ms_bPredSkinLoaded = 0;
        }
        ms_bDisableSaveGame = 0;
        m_Unk81c = 0xe;
        if (MemCardSavesFound() & 0x3f) m_Unk82c = 3;
        if (m_pManager->m_PageState == 4) s_MainGrid->Listener(0x5f5f5f5f5f76616eull)->SetFocusByData(2, 0);
        m_Unk844 = 0;
    case 0x15:
        if (cjmod_confirm_state(1) == 2) s_MainGrid->Object(3, 0)->m_Text = cjmod_ui_str(0);
        // Quit Game row: A/Start here (the menu's own select path skips rows whose data is no state)
        if (s_MainGrid->Listener(0x5f5f5f5f5f76616eull)->Focus() == s_MainGrid->Object(3, 0) &&
            FEListener::QueryButton(0x42000, (FEPad::Flags)0xf, 1)) {
            if (cjmod_confirm(1)) cjmod_quit();
            s_MainGrid->Object(3, 0)->m_Text = cjmod_ui_str(1);
        }
        if (m_Unk82c != 3 && m_Unk82c != 4) {
            s_MainButtonLoad->m_Flags &= ~8;
            s_MainButtonLoad->SetMathmos((FEObjColour::Flags)3, ex::RGBA(0x40, 0x40, 0x40, 0x6e), 1);
        } else {
            if (s_MainButtonLoad->m_Flags & 8) return 0;
            s_MainButtonLoad->m_Flags |= 8;
            s_MainButtonLoad->SetMathmos((FEObjColour::Flags)3, ex::RGBA(0x7f, 0x7f, 0x7f, 0x80), 1);
        }
        return 0;
    case 0x17:
        if (m_UpdateListeners[m_ListenerIdx]->m_pFocus->m_Data == 0x51554954) {
            if (cjmod_confirm(1)) cjmod_quit();
            m_UpdateListeners[m_ListenerIdx]->m_pFocus->m_Text = cjmod_ui_str(1);
            return 1;
        }
        GoState((unsigned int)m_UpdateListeners[m_ListenerIdx]->m_pFocus->m_Data);
        return 1;
    }
    return 0;
}

// MOD 000f2a90
signed char FEMainMenu::UpdateStateOptionsController(FEObjEvent::Flags ev)
{
    FELayout *pCur = m_pCurLayout;
    FEObject *pObj = pCur ? pCur->Object(2) : 0;
    switch (ev) {
    case 0x1e: {
        if (!cjmod_pc_block()) cjmod_pc_attach(new char[0x1000]);
        EXString sEmpty(cjmod_pc_str(3));
        EXString sOff(cjmod_pc_str(4));
        FEObject *pPage = m_pLayout->ObjectAdd(m_ListenerIdx, new FELayoutLayered(4, 0, 0, 0, 0, (EXW::TextAlign)4));
        FEObject *pBack = pPage->ObjectAdd(0, new FEObject(FEObjType::OT_SPRITE, XApp::Frontend2DFile(), 0x6000234, 0xfdffffffull, FE_Blank, FE_Blank,
                                                          (EXW::TextAlign)0, 0, 0, 0, 0, (EXW::TextAlign)4));
        pBack->SetRect(EXRect(0x48, 0xc2, 0x1ea, 0xd7));
        pBack->StateAdd(0x302);
        pBack->SetMathmos((FEObjColour::Flags)3, ex::RGBA(0x23, 0x23, 0x28, 0x50), 1);
        pBack->StateSub(8);
        FELayoutScrollGrid *pGrid = new FELayoutScrollGrid(3, 2, 0x48, 0x3c, 0x1a8, 0x4c, (EXW::TextAlign)0);
        pPage->ObjectAdd(2, pGrid);
        pBack = pPage->ObjectAdd(1, new FEObject(FEObjType::OT_SPRITE, XApp::Frontend2DFile(), 0x6000234, 0xfdffffffull, FE_Blank, FE_Blank,
                                                (EXW::TextAlign)0, 0, 0, 0, 0, (EXW::TextAlign)4));
        EXRect sf = EXBaseDisplay::GetDisplay()->SafeFrame();
        pBack->SetRect(EXRect(sf.x + 0x48, sf.y + 0x3c, 0x1a8, 0x4c));
        pBack->StateAdd(0x302);
        pBack->SetMathmos((FEObjColour::Flags)3, ex::RGBA(0x23, 0x23, 0x28, 0x50), 1);
        pBack->StateSub(8);
        float *pW2 = new float[2];
        pW2[0] = 0.5f;
        pW2[1] = 0.5f;
        pGrid->SetGridWidthProportions(pW2);
        pGrid->SetUpDownScrollArrowForceY(0);
        pGrid->SetVisibleCount(3, 2);
        pGrid->StateSub(8);
        FEObject *pText = AddScrollGridText(pGrid, 0, 0, 0x7974536c72744374ull, Text_Get(0x510005e3), (EXW::TextAlign)2, sEmpty, 1);
        FEObject *pSub = pGrid->ObjectAdd(0, 1, new FELayoutScrollGrid(1, 5, 0, 0, 0, 0, (EXW::TextAlign)4));
        pSub->m_Data = 0x5f7974536c727443ull;
        pText->MathmosLink(pSub);
        FEObject *pFocusObj = 0;
        for (unsigned int i = 0; i < ((XGamePad *)EXBaseGamePad::GetGamePad())->NumConfigs(); i++) {
            EXString name;
            unsigned int cfg;
            switch (i) {
            case 1:
                name = Text_Get(0x510001ff);
                cfg = 1;
                break;
            case 2:
                name = Text_Get(0x51000335);
                cfg = 2;
                break;
            case 3:
                name = Text_Get(0x510003e8);
                cfg = 3;
                break;
            case 4:
                name = Text_Get(0x510003e9);
                cfg = 4;
                break;
            case 5:
                name = Text_Get(0x51000497);
                cfg = 5;
                break;
            case 6:
                name = Text_Get(0x51000499);
                cfg = 6;
                break;
            case 7:
                name = Text_Get(0x5100049b);
                cfg = 7;
                break;
            case 8:
                name = Text_Get(0x5100049d);
                cfg = 8;
                break;
            case 9:
                name = Text_Get(0x5100049f);
                cfg = 9;
                break;
            default:
                name = Text_Get(0x510001fe);
                cfg = 0;
                break;
            }
            FEObject *pItem = pSub->ObjectAdd(0, i, new FEObject(FEObjType::OT_TEXT, 0x100003a, 0x7000004, cfg, name, FE_Blank,
                                                                (EXW::TextAlign)4, 0, 0, 0, 0, (EXW::TextAlign)4))
                                  ->SetHelpText(Text_Get(0x51000474));
            if (cfg == GameData::Instance(1)->Values()->m_ControllerConfig) pFocusObj = pItem;
            ((XGamePad *)EXBaseGamePad::GetGamePad())->SetActions(i);
        }
        pSub->SetVisibleCount(1, 1);
        pSub->Bind((FEDataType::Flags)3, &GameData::Instance(1)->Values()->m_ControllerConfig);
        pText->EnsureVisible(pFocusObj, 0);
        pText = AddScrollGridText(pGrid, 1, 0, 0x6574617262695674ull, Text_Get(0x510001fd), (EXW::TextAlign)2, sEmpty, 1);
        FEObject *pCheck = pGrid->ObjectAdd(1, 1, new FELayoutCheckbox(0, 0, 0, 0, 0, (EXW::TextAlign)4));
        pCheck->m_Data = 0x5f65746172626956ull;
        pCheck->Bind((FEDataType::Flags)1, &GameData::Instance(1)->Values()->m_Unk1c[3]);
        pCheck->SetHelpText(Text_Get(0x510004d7));
        pText->MathmosLink(pCheck);
        pText = AddScrollGridText(pGrid, 2, 0, 0x5f6e61654c504674ull, Text_Get(0x510007fd), (EXW::TextAlign)2, sEmpty, 1);
        pSub = pGrid->ObjectAdd(2, 1, new FELayoutScrollGrid(1, 3, 0, 0, 0, 0, (EXW::TextAlign)4));
        pSub->m_Data = 0x5f5f6e61654c5046ull;
        pText->MathmosLink(pSub);
        pSub->ObjectAdd(0, 0, new FEObject(FEObjType::OT_TEXT, 0x100003a, 0x7000004, 0, sOff, FE_Blank,
                                           (EXW::TextAlign)4, 0, 0, 0, 0, (EXW::TextAlign)4))
            ->SetHelpText(Text_Get(0x51000474));
        pSub->ObjectAdd(0, 1, new FEObject(FEObjType::OT_TEXT, 0x100003a, 0x7000004, 1, Text_Get(0x510007fe), FE_Blank,
                                           (EXW::TextAlign)4, 0, 0, 0, 0, (EXW::TextAlign)4))
            ->SetHelpText(Text_Get(0x51000474));
        pSub->ObjectAdd(0, 2, new FEObject(FEObjType::OT_TEXT, 0x100003a, 0x7000004, 2, Text_Get(0x510007ff), FE_Blank,
                                           (EXW::TextAlign)4, 0, 0, 0, 0, (EXW::TextAlign)4))
            ->SetHelpText(Text_Get(0x51000474));
        pSub->SetVisibleCount(1, 1);
        pSub->Bind((FEDataType::Flags)2, &GameData::Instance(1)->Values()->m_ButtonConfig);
        // PC: a flat modern pad picture (the pause menu's built-in art, src/texpack.c) with the labels
        // in two columns at its leader line ends, in place of the 3D Xbox pad and its datum labels.
        cjmod_ctrl_attach(new char[40 * 64]);
        FEObject *pLay = pPage->ObjectAdd(3, new FELayoutLayered(3, 0x26, 0xae, 0x1d0, 0xc4, (EXW::TextAlign)0));
        pLay->ObjectAdd(0, new FEObject((FEObjType::Flags)1, 0x1000025, 0x86000076, 0xfdffffff, FE_Blank, FE_Blank,
                                        (EXW::TextAlign)4, 0, 0, 0x1d0, 0xc4, (EXW::TextAlign)4));
        FELayoutScrollGrid *pSG = new FELayoutScrollGrid(9, 3, 0, 0, 0x1d0, 0xc4, (EXW::TextAlign)4);
        float *pW = new float[3];   // the pause page's size, a little shorter to clear the footer
        pW[0] = 0.34f;
        pW[1] = 0.32f;
        pW[2] = 0.34f;
        pSG->SetGridWidthProportions(pW);
        pSG->SetVisibleCount(9, 3);
        pSG->StateSub(8);
        FEObject *pLabels = pLay->ObjectAdd(1, pSG);
        cjmod_ctrl_grid((char *)pLay);
        for (int k = 0; k < 18; k++)
            pLabels->ObjectAdd(k % 9, k < 9 ? 0 : 2,
                              new FEObject((FEObjType::Flags)2, 0x100003a, 0x700000a, 0xfdffffff, FE_Blank, FE_Blank,
                                           (EXW::TextAlign)(k < 9 ? 7 : 1), 0, 0, 0, 0, (EXW::TextAlign)4));
        // keyboard and mouse: the 11-row list with rebinding (as on the pause page)
        FELayoutScrollGrid *pKG = new FELayoutScrollGrid(11, 3, 0, 0, 0x1d0, 0xc4, (EXW::TextAlign)4);
        float *pKW = new float[3];
        pKW[0] = 0.46f;
        pKW[1] = 0.08f;
        pKW[2] = 0.46f;
        pKG->SetGridWidthProportions(pKW);
        pKG->SetVisibleCount(11, 3);
        pKG->StateSub(8);
        FEObject *pKeys = pLay->ObjectAdd(2, pKG);
        for (int k = 0; k < 22; k++) {
            FEObject *pCell = pKeys->ObjectAdd(cjmod_kb_row(k), cjmod_kb_col(k),
                                               new FEObject((FEObjType::Flags)2, 0x100003a, 0x700000a, 0xfdffffff, FE_Blank, FE_Blank,
                                                            (EXW::TextAlign)(cjmod_kb_col(k) == 0 ? 7 : 1), 0, 0, 0, 0,
                                                            (EXW::TextAlign)4));
            pCell->SetMathmos((FEObjColour::Flags)0,
                              FEMathmos(ex::RGBA(0x7f, 0, 0, 0x80), ex::RGBA(0x7f, 0x7f, 0x7f, 0x80), 60.0f, FEWaveform::WF_LINEAR, 0), 1);
            pCell->SetMathmos((FEObjColour::Flags)1, FEMathmos(ex::RGBA(0x7f, 0x7f, 0x7f, 0x80)), 1);
        }
        pLay->StateSub(8);
        return 0;
    }
    case 0x23:
        SetTitleAndBackgr(0, EXRect(0, 0, 0, 0));
    case 0x15:
        if (m_b1) {
            ((XGamePad *)EXBaseGamePad::GetGamePad())->SetActions(GameData::Instance(1)->Values()->m_ControllerConfig);
            cjmod_ctrl_page_alive();
            FEObject *pLayC = (FEObject *)cjmod_ctrl_grid(0);
            FEObject *pLabs = pLayC ? pLayC->Object(1) : 0;
            FEObject *pKeysC = pLayC ? pLayC->Object(2) : 0;
            for (int k = 0; k < 22; k++) {
                EXString a = ((XGamePad *)EXBaseGamePad::GetGamePad())->GetControllerString(cjmod_ctrl_hash(k < 18 ? k : 0));
                if (k < 18) {
                    FEObject *pCell = pLabs ? pLabs->Object(k % 9, k < 9 ? 0 : 2) : 0;
                    if (pCell) pCell->m_Text = cjmod_ctrl_text(k, a.c_str());
                }
                FEObject *pKC = pKeysC ? pKeysC->Object(cjmod_kb_row(k), cjmod_kb_col(k)) : 0;
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
        return 0;
    case 0x1f: {
        pObj->ListenerAdd(new FEListener(0x5f5f5f5f5f76616eull, 0xc03, MainMenu_Callback, (FEPad::Flags)0xf));
        FEListener *pl = pObj->Listener(0x5f5f5f5f5f76616eull);
        SetUpdateListener(pl);
        pObj->Listener(0x5f5f5f5f5f76616eull)->SetFocusByData(GameData::Instance(1)->Values()->m_ControllerConfig, 0);
        pObj->SetMathmos((FEObjColour::Flags)0,
                         FEMathmos(ex::RGBA(0x7f, 0, 0, 0x80), ex::RGBA(0x7f, 0x7f, 0x7f, 0x80), 60.0f, FEWaveform::WF_LINEAR, 0), 1);
        pObj->SetMathmos((FEObjColour::Flags)1, FEMathmos(ex::RGBA(0x7f, 0x7f, 0x7f, 0x80)), 1);
        return 0;
    }
    case 0x21:
        pObj->ListenerClear();
        break;
    }
    return 0;
}
