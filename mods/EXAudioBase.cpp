// Mod: keep a music start request that arrives while the old music fades out to stop.
// A fade-and-stop on the music fader ends in ES_StopMusic, which also cancels any pending
// start request (StartMusicData). The level-load fade-out of the menu / loading music is one:
// on a fast PC the chapter posts its own track (SFXStartMusicXFade) before that fade ends,
// the end of the fade wipes the request, and the chapter intro plays without music until
// the game asks again some 15 s later. The original even warns about the case in its debug
// build ("Music Started during FadeAndStop"). Here the old music still stops; a request
// posted in the meantime survives and starts on the next audio update (ES_TryPlayMusic).
// When nothing is pending it is the original function.

// MOD 002c48a0
void EXAudio_Fader::FaderEndUpdate(int bank)
{
    if (m_Bank) {
        gpEXAudio->ES_UnLoadSoundBankRelease(bank);
        return;
    }
    if (m_Stop) {
        if (m_Music) {
            if (StartMusicData.m_Start && StartMusicData.m_Hash != -1) {
                _STARTMUSICDATA keep = StartMusicData;
                ES_StopMusic();
                StartMusicData = keep;
            } else {
                ES_StopMusic();
            }
        } else {
            gpEXAudio->SFXRemove(0, -1);
        }
    } else {
        if (m_Music) {
            ES_MusicPause();
        } else {
            for (SFXItem *p = gpEXAudio->m_pSFXList; p; p = p->m_pNext) {
                if (p->m_Pausable) p->Pause();
            }
        }
    }
}
