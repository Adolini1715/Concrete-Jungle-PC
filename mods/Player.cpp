// Mods on Player.obj (pcj-decomp src/game/Player.cpp). Functions listed under mod=Player in
// tools/hybridize/targets.txt without a section here run the decomp source as it is.
//
// MoveNormal: the run, with movement dials (pcj.ini, percent, 100 = original;
// src/native/hybrid/cjmod_move.c): CJ_RUN_ACCEL scales the speed-up, CJ_RUN_STOP the slow-down
// (the original keeps 93.75% of the speed each frame when stopping: the slide),
// CJ_TURN_RATE the running turn. All stay frame-rate correct (exponents on the per-frame factors).
extern "C" void cjmod_grab_log(int what, int flags, float a, float b, float mul, float probe_y);
extern "C" float cjmod_run_accel(void);
extern "C" float cjmod_run_stop(void);
extern "C" float cjmod_turn_rate(void);
extern "C" int cjmod_snappy_stop(void);
extern "C" unsigned int cjmod_anim_test(void);
float InterpAngle(float a, float b, float t, float p);
extern "C" int cjmod_move_modern(void);
extern "C" float cjmod_charge_move(void);
extern "C" int cjmod_roll_pressed_within(int ms);
extern "C" void cjmod_roll_consume(void);
extern "C" int cjmod_roll_window_ms(void);
extern "C" void cjmod_roll_start(float sx, float sz);
extern "C" int cjmod_roll_active(void);
extern "C" void cjmod_roll_end(void);
extern "C" float cjmod_roll_vx(void);
extern "C" float cjmod_roll_vz(void);
extern "C" void cjmod_hard_land(float health_before, float vx, float vz);
extern "C" int cjmod_hard_land_recent(int ms);
extern "C" float cjmod_hard_land_health(void);
extern "C" float cjmod_hard_land_vx(void);
extern "C" float cjmod_hard_land_vz(void);
extern "C" void cjmod_hard_land_clear(void);
extern "C" void cjmod_trace_charge(float stick, float pad, float speed, unsigned flags, float timer);
#define CJ_QUICK_LAND_MODE 0x09000270u   /* AnimMode UN_Land_To_Run_Quick (unused in the retail code) */
extern "C" float cjmod_quick_land_min(void);
extern "C" float cjmod_run_curve(float f);
extern "C" float cjmod_run_inv(float g);
extern "C" float cjmod_ground(float f);
extern "C" float cjmod_run_remap(float mag);
extern "C" int cjmod_run_charging(void);
extern "C" void cjmod_run_charge_touch(int on);
extern "C" float cjmod_charge_cap(float mag);
extern "C" void cjmod_crouch_hold_start(void);
extern "C" int cjmod_crouch_hold(float dt);
extern "C" void cjmod_charge_feedback(float timer, int power_ok, int medium_ok);
extern "C" float cjmod_attack_carry(float mul);
extern "C" float cjmod_turn_mult(void);
extern "C" float cjmod_turn_cost(void);
extern "C" float cjmod_air_push(void);
extern "C" float cjmod_stop_frac(void);

// MOD 00165450
signed char XItemHandler_Player::MoveNormal(int flags)
{
    EXPlacementItem *pItem = m_pItem;
    EXVector rot = pItem->Rotation();
    float diff = Vec_AngleDifference(rot.y, m_PadAngle);
    m_PadHistory[0] = m_PadHistory[1];
    m_PadHistory[1] = m_PadHistory[2];
    m_PadHistory[2] = m_PadHistory[3];
    m_PadHistory[3].m_Angle = m_PadAngle;
    m_PadHistory[3].m_Update = m_UpdateCount;
    float reversal = 0.0f;
    if (m_UpdateCount - m_PadHistory[0].m_Update == 3) reversal = (float)fabs(Vec_AngleDifference(m_PadHistory[0].m_Angle, m_PadAngle));
    float mag = m_StickMag;
    if (flags & 1) mag = 0.0f;
    if (flags & 4) mag = mag > 0.5f ? 0.5f : mag;
    mag = cjmod_run_remap(mag);   // modern: the smooth speed curve
    mag = cjmod_charge_cap(mag);  // modern: running while holding jump (charging) at CJ_CHARGE_MOVE
    float pivot = 1.0f;
    if (reversal > 1.7453293f && m_MSpeedTarget > 0.09f && cjmod_move_modern()) {
        // modern: no skid animation; a fast pivot that keeps about half the speed (weight)
        pivot = 4.0f;
        m_Speed.x *= 0.5f;
        m_Speed.z *= 0.5f;
    } else if (reversal > 1.7453293f && m_MSpeedTarget > 0.09f) {
        XItemPhysics *pPhys = (XItemPhysics *)m_pItem->m_pItemPhysics;
        m_Speed = EXVector(0.0f, 0.0f, 0.0f, 0.0f);
        m_Unk2b8 = EXVector(0.0f, 0.0f, 0.0f, 0.0f);
        pPhys->m_DeltaMove = EXVector(0.0f, 0.0f, 0.0f, 0.0f);
        EXPlacementItem *pI = m_pItem;
        pI->m_rotation = EXVector(0.0f, pI->Rotation().y, 0.0f, 0.0f);
        pI->m_validMtx = 0;
        m_SkidTurn = Vec_AngleDifference(rot.y + 3.1415927f, m_PadAngle) * 0.045454547f;
        SetupMove(0xe, diff < 0.0f ? 0 : 1, -1.0f);
        return 1;
    }
    float turn = InterpAngle(0.0f, diff, 0.12f * cjmod_turn_rate() * cjmod_turn_mult() * pivot, XApp::GameFrameRateMul());
    rot.y += turn;
    float bank = diff * -0.16666667f;
    if (bank >= 0.3926991f) bank = 0.3926991f;
    else if (bank <= -0.3926991f) bank = -0.3926991f;
    float bd = Vec_AngleDifference(m_MSpeedTarget * 5.333447f * bank, rot.z);
    float lim = XApp::GameFrameRateMul() * 0.061086524f;
    float step = bd < lim ? (-lim < bd ? bd : -lim) : lim;
    pItem = m_pItem;
    pItem->m_rotation = EXVector(rot.x, rot.y, rot.z - step, rot.w);
    pItem->m_validMtx = 0;
    float t2 = cjmod_turn_cost() == 2.0f ? (float)fabs(turn) + (float)fabs(turn) : (float)fabs(turn) * cjmod_turn_cost();
    if (t2 > 1.0f) mag /= t2;
    float speed = sqrtf(m_Speed.x * m_Speed.x + m_Speed.z * m_Speed.z);
    float ns;
    if (mag + 0.0001f < speed) ns = (float)pow(0.9375, (double)(XApp::GameFrameRateMul() * cjmod_run_stop())) * speed;
    else ns = (speed - mag) * (float)pow(0.7999999970197678, (double)(XApp::GameFrameRateMul() * cjmod_run_accel())) + mag;
    if (ns < mag) ns = ns > 0.0f ? ns : 0.0f;
    else ns = mag;
    pItem = m_pItem;
    m_Speed.x = (float)sin(pItem->Rotation().y) * ns;
    m_Speed.z = (float)cos(pItem->Rotation().y) * ns;
    if (m_Unk214 == 1) {
        if (flags & 8) return CheckForFall() != 0;
        CheckChainButtonPress();
        if (m_bAttacking && (m_AttackHit == 1 || m_AttackHit == 3)) {
            SetWeapon(m_AttackHit);
            m_Unk224 = 2.0f;
            m_AttackState = 1;
            m_AttackHit = -1;
            m_bAttacking = 0;
            m_HitMoveScale = 1.0f;
            SetupMove(6, 0, -1.0f);
            EXPlacementItem *pI = m_pItem;
            EXVector r = pI->Rotation();
            pI->m_rotation = EXVector(r.x, r.y, 0.0f, r.w);
            pI->m_validMtx = 0;
            return 0;
        }
        m_bAttacking = 0;
    }
    if (!(flags & 8) && CheckForJump(1)) return 1;
    return CheckForFall() != 0;
}

// CheckForStop: with CJ_SNAPPY_STOP=1, letting go of the stick while running fast stops on
// the spot (motion zeroed, straight to idle) instead of playing the run-stop animation
// (mode 0, submode 9/10), whose root motion is most of the "ice rink" slide. Slow stops,
// carrying an actor (hold 1) and the hold-2 variant are unchanged. Off: the original.
// MOD 00160760
signed char XItemHandler_Player::CheckForStop(int hold)
{
    if (m_StickMag == 0.0f) {
        const EXVector &r = m_pItem->Rotation();
        m_pItem->SetRotation(EXQuaternion(r.x, r.y, 0.0f, r.w));
        if (m_MSpeedTarget < 0.18f) {
            switch (hold) {
            case 0: SetupMove(0, 0, -1.0f); break;
            case 1: SetupMove(0x16, 1, -1.0f); break;
            case 2: SetupMove(1, 6, -1.0f); break;
            }
        } else {
            switch (hold) {
            case 0:
                if (cjmod_snappy_stop()) {
                    DeadStop(0);
                    SetupMove(0, 0, -1.0f);
                } else if (m_ShimmyDir < 1.5f)
                    SetupMove(0, 9, -1.0f);
                else
                    SetupMove(0, 10, -1.0f);
                break;
            case 1: SetupMove(0x16, 2, -1.0f); break;
            case 2: SetupMove(1, 7, -1.0f); break;
            }
        }
        return 1;
    }
    return 0;
}

// DoIdle: the original, plus CJ_ANIM_TEST (testing: play animation modes by hash while idle).
// MOD 00168600
void XItemHandler_Player::DoIdle()
{
    if (gCurrentMap_GeoHash == XApp::Frontend3DFile()) return;
    if (cjmod_roll_active()) {   // modern movement: a quick landing in progress (CheckForLand)
        if (!AnimEnded() && m_Health > 0.0f) {
            cjmod_trace_charge(-1.0f, ((EXItemAnimator_Anim *)m_pItem->GetAnimator(EXRUNTIME_CLASS(EXItemAnimator_Anim)))->Controller(0)->Frame(), XApp::FrameLength(), 0x270u, XApp::GameFrameRateMul());
            m_Speed.x = cjmod_roll_vx();
            m_Speed.z = cjmod_roll_vz();
            m_Speed.y = 0.0f;
            m_Speed.w = 0.0f;
            CheckForFall();
            return;
        }
        cjmod_roll_end();
        SetupMove(m_StickMag > 0.0f ? 1 : 0, 0, -1.0f);   // straight back into the run
        return;
    }
    {   // CJ_ANIM_TEST (src/native/hybrid/cjmod_move.c): play an animation mode by hash
        unsigned int t = cjmod_anim_test();
        if (t) {
            ForceModeChange(t);
            m_SubMode = 11;     // when it ends, DoIdle's case 11 returns to idle
            if (t == CJ_QUICK_LAND_MODE) cjmod_roll_start(0.0f, 0.0f);   // exactly the quick landing's path (timing tests)
            return;
        }
    }
    if (m_Health <= 0.0f) {
        KillPlayer();
        return;
    }
    if (CheckForFall()) return;
    if (!m_HoldState) {
        if (m_bIdleHitReact) {
            m_Unk224 = m_HitAngle;
            SetupMove(0, 8, -1.0f);
            m_bIdleHitReact = 0;
            return;
        }
        if (CheckForIdleBreakOut(1)) return;
        if ((m_ButtonsPressed & 0x10) && CheckForFlipGrab()) return;
    }
    if (m_SubMode >= 5 && m_SubMode <= 7 && m_PlasmaChargeTime > 0.0f) {
        SetupMove(0, 0, -1.0f);
        m_IdleTime = 0.0f;
        return;
    }
    if (CheckForIdleBreakOut(6)) return;
    if ((m_SubMode == 9 || m_SubMode == 10) && cjmod_move_modern() &&
        ((EXItemAnimator_Anim *)m_pItem->GetAnimator(EXRUNTIME_CLASS(EXItemAnimator_Anim)))->Controller(0)->Frame() > cjmod_stop_frac()) {
        // modern: a short stop (the first part of the run-stop animation, then idle)
        m_Speed.SetZero();
        SetupMove(0, 0, -1.0f);
        return;
    }
    if (!AnimEnded()) return;
    switch (m_SubMode) {
    case 11:
        if (m_HoldState)
            SetupMove(0x16, 1, -1.0f);
        else
            SetupMove(0, 0, -1.0f);
        return;
    case 0:
    case 1: {
        signed char b = 1;
        if (m_IdleTime < 10.0f) b = 0;
        if (gCurrentMap_GeoHash == 0x1000070) b = 0;
        if (XGetCamera(0)->IsFirstPerson()) b = 0;
        if (!(m_PlasmaChargeTime > 0.0f) && b) SetupMove(0, 5, -1.0f);
        return;
    }
    case 5: {
        if (XGetCamera(0)->IsFirstPerson()) return;
        if (g_EXRandClass.Rand32() % 21 != 10) return;
        int n;
        if (m_SkinBC == 12 || m_SkinBC == 13)
            n = 1;
        else
            n = GetCardRandom() & 1;
        switch (n) {
        case 0:
            SetupMove(0, 6, -1.0f);
            SetWeapon(3);
            break;
        case 1:
            SetupMove(0, 7, -1.0f);
            break;
        }
        return;
    }
    case 8:
    case 9:
    case 10:
        SetupMove(0, 0, -1.0f);
        return;
    case 6:
    case 7:
        SetupMove(0, 5, 31.0f);
        return;
    }
}

// Modern movement (docs/movement-design.md; CJ_MOVE_STYLE=classic: the original).
// CheckForJump: a jump pressed while running goes into the charge (mode 2) with the run's speed
// instead of stopping dead for the running wind-up (DeadStop + mode 0x10).
// MOD 0015faa0
signed char XItemHandler_Player::CheckForJump(signed char b)
{
    if (b && cjmod_move_modern() && cjmod_run_charging()) {
        // running charge: keep running while the button is held; on release crouch briefly, then jump
        if (m_ButtonsPressed & 0x20) {
            m_CrouchTimer += XApp::FrameLength();
            cjmod_run_charge_touch(1);
            cjmod_charge_feedback(m_CrouchTimer, (m_PredMsgFlags & 2) != 0, (m_PredMsgFlags & 0x10) != 0);
            return 0;
        }
        cjmod_run_charge_touch(0);
        if (m_CrouchTimer >= 0.1f) cjmod_crouch_hold_start();   // a real charge: show the crouch first; a tap jumps at once
        SetupMove(2, 0, -1.0f);     // DoCrouch launches on m_CrouchTimer (after the release crouch, if any)
        DeleteAllSwoosh();
        return 1;
    }
    if ((m_PredMsgFlags & 0x20) && (m_ButtonsHeld & 0x20)) {
        XItemPhysics *pPhys = (XItemPhysics *)m_pItem->m_pItemPhysics;
        if (m_PredMsgFlags & 0x10) {
            m_JumpBaseY = m_pItem->Position().y;
            m_Unk344 = 12.0f;
        }
        if (m_MSpeedTarget < 0.01f) {
            pPhys->SetThrustToAttainHeight(3.217f);
            SetupMove(3, 0, -1.0f);
        } else if (b && cjmod_move_modern()) {
            m_CrouchTimer = 0.0f;
            cjmod_run_charge_touch(1);  // start charging, keep running (handled above from the next frame)
            return 0;
        } else if (b) {
            DeadStop(1);
            SetupMove(0x10, 0, -1.0f);
        } else {
            pPhys->SetThrustToAttainHeight(3.217f);
            SetupMove(3, 1, -1.0f);
        }
        DeleteAllSwoosh();
        return 1;
    }
    return 0;
}

// DoCrouch (the jump charge): modern movement keeps moving at CJ_CHARGE_MOVE (60%) of the
// stick's run speed, turning like the run, and the jump keeps that speed. Classic: rooted.
// MOD 001689c0
void XItemHandler_Player::DoCrouch()
{
    int modern = cjmod_move_modern();
    if (modern && m_StickMag > 0.1f && (m_ButtonsPressed & 0x20) && !cjmod_crouch_hold(0.0f)) {
        // a standing charge and the stick pushed: get up and run, the charge carries on (CheckForJump, running charge)
        cjmod_run_charge_touch(1);
        SetupMove(1, 0, -1.0f);
        return;
    }
    if (modern) {
        EXPlacementItem *pItem = m_pItem;
        EXVector rot = pItem->Rotation();
        float mag = cjmod_run_inv(cjmod_ground(m_StickMag) * cjmod_charge_move());
        if (m_StickMag > 0.0f) {
            float turn = InterpAngle(0.0f, Vec_AngleDifference(rot.y, m_PadAngle), 0.12f, XApp::GameFrameRateMul());
            rot.y += turn;
            pItem->m_rotation = EXVector(rot.x, rot.y, rot.z, rot.w);
            pItem->m_validMtx = 0;
        }
        float speed = sqrtf(m_Speed.x * m_Speed.x + m_Speed.z * m_Speed.z);
        float ns;
        if (mag + 0.0001f < speed) ns = (float)pow(0.9375, (double)XApp::GameFrameRateMul()) * speed;
        else ns = (speed - mag) * (float)pow(0.7999999970197678, (double)XApp::GameFrameRateMul()) + mag;
        if (ns < mag) ns = ns > 0.0f ? ns : 0.0f;
        else ns = mag;
        m_Speed.x = (float)sin(rot.y) * ns;
        m_Speed.z = (float)cos(rot.y) * ns;
        m_Speed.y = 0.0f;
        m_Speed.w = 0.0f;
        cjmod_trace_charge(m_StickMag, m_PadAngle, ns, (unsigned)m_MoveFlags, m_CrouchTimer);
    } else
        m_Speed.SetZero();
    if (m_bAnimEnded) {
        if (m_SubMode == 0 || (m_SubMode > 1 && m_SubMode <= 3)) SetupMove(2, 1, -1.0f);
    }
    if (m_CrouchTimer >= 0.33333334f && !(modern && m_StickMag > 0.0f)) {
        EXQuaternion q = m_pItem->Rotation();
        float d = Vec_AngleDifference(q.y, m_PadAngle);
        q.y += d > 0.0f ? min(d, XApp::GameFrameRateMul() * 0.04363323f) : max(d, XApp::GameFrameRateMul() * -0.04363323f);
        m_pItem->SetRotation(q);
        if (m_SubMode <= 1) {
            if (d > 0.0017453292f && m_SubMode != 3) SetupMove(2, 3, -1.0f);
            if (-0.0017453292f > d && m_SubMode != 2) SetupMove(2, 2, -1.0f);
        }
    }
    if (modern && cjmod_crouch_hold(XApp::FrameLength())) {   // the release crouch of a running charge
        CheckForFall();
        return;
    }
    if (m_ButtonsPressed & 0x20) {
        m_CrouchTimer += XApp::FrameLength();
        if (modern) cjmod_charge_feedback(m_CrouchTimer, (m_PredMsgFlags & 2) != 0, (m_PredMsgFlags & 0x10) != 0);
        CheckForFall();
        return;
    }
    XItemPhysics *pPhys = (XItemPhysics *)m_pItem->m_pItemPhysics;
    int moving = modern && sqrtf(m_Speed.x * m_Speed.x + m_Speed.z * m_Speed.z) > 0.09f;
    if (!modern) m_Speed.SetZero();
    if (m_CrouchTimer >= 0.33333334f && (m_PredMsgFlags & 2)) {
        pPhys->SetThrustToAttainHeight(10.0f);
        SetupMove(3, 2, -1.0f);
    } else if (m_CrouchTimer >= 0.1f && (m_PredMsgFlags & 0x10)) {
        pPhys->SetThrustToAttainHeight(6.507f);
        SetupMove(3, moving ? 1 : 0, -1.0f);
    } else if (m_PredMsgFlags & 0x20) {
        pPhys->SetThrustToAttainHeight(3.217f);
        SetupMove(3, moving ? 1 : 0, -1.0f);
    } else
        SetupMove(0, 0, -1.0f);
    m_CrouchTimer = 0.0f;
    CheckForFall();
}

// CheckForLand: the decomp's, plus the quick landing (modern movement): a hard landing from
// anything but a very high fall (impact slower than CJ_QUICK_LAND_MAX, default 1.3/frame, about
// a 50-unit drop) plays the unused land-to-run animation (0.57 s) instead of the 2-second
// recovery. Fall damage as in the original.
// MOD 0016a400
void XItemHandler_Player::CheckForLand()
{
    XItemPhysics *pPhys = (XItemPhysics *)m_pItem->m_pItemPhysics;
    if (!(pPhys->m_collisionFlags & 1)) return;
    float speed = 1.0f;
    if (0.18f > m_MSpeedTarget) speed = 0.9f;
    if (0.09f > m_MSpeedTarget) speed = 0.45f;
    if (0.01f > m_MSpeedTarget) speed = 0.0f;
    float a = (float)atan2(m_Unk2b8.x, m_Unk2b8.z);
    m_Speed.w = 0.0f;
    m_Speed.y = 0.0f;
    m_Speed.x = (float)sin(a) * speed;
    m_Speed.z = (float)cos(a) * speed;
    m_bLedgeTarget = 0;
    m_Unk35c = 0.0f;
    m_Unk358 = 0.0f;
    m_FallTimer = 0.0f;
    m_Unk2b8 = EXVector(0.0f, 0.0f, 0.0f, 0.0f);
    EXItem *pItem = m_pItem;
    gpGameWnd[0]->SwooshSysMgr()->DeleteAllSystems(pItem);
    pPhys->m_physicsFlags &= ~3;
    m_CamAngle = XGetCamera(m_PlayerIndex)->Angle();
    if (m_Health <= 0.0f) {
        SetupMove(0, 0, -1.0f);
        KillPlayer();
        return;
    }
    EXPlacementItem *pI = m_pItem;
    pI->m_rotation = EXVector(0.0f, pI->Rotation().y, 0.0f, pI->Rotation().w);
    pI->m_validMtx = 0;
    if (theApp.m_FadeActive) {
        SetupMove(0, 0, -1.0f);
        return;
    }
    int sound;
    if (!CheckForActorStomp()) {
        float v = pPhys->m_ImpactSpeed;
        if (v >= -0.69f) {
            if (m_StickMag <= 0.3f) {
                float f = v;
                if (f < 0.0f) { if (f <= -0.66f) f = -0.66f; }
                else f = 0.0f;
                m_ShimmyDir = ((0.0f - f) - 0.25f) * 2.4390244f + 1.0f;
                SetupMove(0, 0xb, -1.0f);
            } else if (!m_HoldState) {
                SetupMove(1, v >= -0.3f ? 0 : 2, -1.0f);
            } else {
                SetupMove(0x16, 3, -1.0f);
            }
            sound = 2;
            goto landed;
        }
        InflictDamageFromFall(v);
        if (cjmod_move_modern() && !m_HoldState && m_Health > 0.0f && v >= cjmod_quick_land_min()) {
            // quick landing (modern movement): the game's own landing for medium drops (the one every
            // ordinary jump uses) instead of the 2-second hard-landing recovery; full fall damage, the
            // hard landing's dust, sound and rumble. (The unused land-to-run clip, 0x09000270, did not
            // play in the game: the fall pose froze while he slid, 2026-10-28 session.)
            if (m_StickMag <= 0.3f) {
                m_ShimmyDir = (0.66f - 0.25f) * 2.4390244f + 1.0f;   // the heaviest stand landing
                SetupMove(0, 0xb, -1.0f);
            } else
                SetupMove(1, 2, -1.0f);                               // the heavy running landing
            sound = 3;
            goto landed;
        }
    }
    SetupMove(8, 0, -1.0f);
    sound = 3;
landed:
    EXBaseSoundManager::GetSoundManager()->StartSound(GroundSounds[m_FloorType][sound], m_pItem->Position(), -1, 0);
    {
        EXVector feet = GetMidFeetPos();
        feet.y = m_pItem->Position().y;
        if (sound == 2) {
            if (m_Wet) XItemHandler_ScriptBase::FX_Create(feet, 0x4000519, 0x40, 0x1000016);
        } else if (sound == 3) {
            if (m_bCloak) SparkSkin(0.5f);
            m_RefractionBoost = 0.5f;
            Actor::OverridePlayerCloak();
            XItemHandler_ScriptBase::Explosion_Create(feet, m_Wet ? 0x4000518 : 0x40002d7, 3, 3, 0x1e, 0, 0, 0, 0);
            ((XGamePad *)EXBaseGamePad::GetGamePad())->PlayRumble(0x54000000, 1.0f, m_PadIndex);
        }
    }
}


// MoveJump: the decomp's; modern movement scales the stick's push in a free jump (air control,
// CJ_AIR_CONTROL percent, default 250). The horizontal cap is the original's.
// MOD 00150960
// and roll level out. Horizontal motion is carried in m_Unk2b8/m_Speed, not DeltaMove.
void XItemHandler_Player::MoveJump()
{
    XItemPhysics *pPhys = (XItemPhysics *)m_pItem->m_pItemPhysics;
    float yaw = m_pItem->Rotation().y;
    float limit;
    EXVector target;

    if (m_bJumpTarget) {
        limit = 0.25f;
        target = m_JumpTarget;
        int type;
        EXItem *pPlat = GetPlatformItem(type);
        if (pPlat && type > 0 && type <= 2) {
            EXDatumTemp d;
            if (!pPlat->GetAnimatorDatum(0x10000060, d, 1, 0)) memset(&d, 0, sizeof d);
            EXVector r;
            d.m_rotation.GetRotation(r, 4);
            yaw = type == 1 ? r.y + 1.5707964f : r.y;
        }
    } else if (m_bLedgeTarget) {
        limit = 0.09375f;
        target = m_LedgeTarget;
        yaw = m_LedgeAngle;
    } else {
        // free jump
        float mul;
        EXVector push;
        m_Unk2b8 = pPhys->m_DeltaMove;
        m_Unk2b8.w = 0.0f;
        m_Unk2b8.y = 0.0f;
        push.x = 0.0f;
        push.z = 0.0f;
        if (!(m_MoveFlags & 1) && 0.001f > pPhys->m_CollisionResponse.y) {
            float h = sqrtf(m_Pad.x * m_Pad.x + m_Pad.z * m_Pad.z) * cjmod_air_push();
            push.x = h * (float)sin(m_PadAngle);
            push.z = (float)cos(m_PadAngle) * h;
        }
        mul = XApp::GameFrameRateMul();
        m_Unk2b8.x += push.x * mul;
        m_Unk2b8.y += mul * 0.0f;
        m_Unk2b8.z += push.z * mul;
        m_Unk2b8.w += mul * 0.0f;
        if (sqrtf(m_Unk2b8.x * m_Unk2b8.x + m_Unk2b8.z * m_Unk2b8.z) > 0.187496f) {
            float l = sqrtf(m_Unk2b8.x * m_Unk2b8.x + m_Unk2b8.y * m_Unk2b8.y + m_Unk2b8.z * m_Unk2b8.z + m_Unk2b8.w * m_Unk2b8.w);
            if (l > 1e-5f) {
                float k = 1.0f / l;
                m_Unk2b8.x *= k; m_Unk2b8.y *= k; m_Unk2b8.z *= k; m_Unk2b8.w *= k;
            }
            m_Unk2b8.x *= 0.187496f; m_Unk2b8.y *= 0.187496f; m_Unk2b8.z *= 0.187496f; m_Unk2b8.w *= 0.187496f;
        }
        if (sqrtf(m_Unk2b8.x * m_Unk2b8.x + m_Unk2b8.z * m_Unk2b8.z) > 0.1f && !(m_MoveFlags & 2)) {
            float rate = 0.1f;
            if (m_Mode == 0xa && m_SubMode == 2) rate = m_bUnk370 ? 0.09f : 0.0f;
            rate *= XApp::GameFrameRateMul();
            float a = (float)atan2(m_Unk2b8.x, m_Unk2b8.z);
            if (m_MoveFlags & 8) a += 3.1415927f;
            EXPlacementItem *pItem = m_pItem;
            float diff = Vec_AngleDifference(pItem->Rotation().y, a);
            if ((float)fabs(diff) < 6.2831855f) {
                float step = diff > rate ? rate : (diff < -rate ? -rate : diff);
                EXVector rot = pItem->Rotation();
                rot.y += step;
                pItem->m_rotation = rot;
                pItem->m_validMtx = 0;
            }
        }
        if (!(m_HandLock > 0.0f)) {
            EXPlacementItem *pItem = m_pItem;
            EXVector rot = pItem->Rotation();
            float r = XApp::GameFrameRateMul() * 0.18f;
            float dx = Vec_AngleDifference(rot.x, 0.0f);
            float sx = dx >= r ? r : (-r >= dx ? -r : dx);
            float dz = Vec_AngleDifference(rot.z, 0.0f);
            float sz = dz >= r ? r : (-r >= dz ? -r : dz);
            rot.x += sx;
            rot.z += sz;
            pItem->m_rotation = rot;
            pItem->m_validMtx = 0;
        }
        pPhys->m_DeltaMove.x = 0.0f;
        pPhys->m_DeltaMove.z = 0.0f;
        pPhys->m_DeltaMove.w = 0.0f;
        m_Unk2b8.w = 0.0f;
        m_Unk2b8.y = 0.0f;
        return;
    }

    // homing onto the target: turn
    {
        EXPlacementItem *pItem = m_pItem;
        EXVector rot = pItem->Rotation();
        float diff = Vec_AngleDifference(rot.y, yaw);
        float r = XApp::GameFrameRateMul();
        float step;
        if (diff > 0.0f) { r *= 0.08726646f; step = r < diff ? r : diff; }
        else { r *= -0.08726646f; step = diff >= r ? diff : r; }
        rot.y += step;
        pItem->m_rotation = rot;
        pItem->m_validMtx = 0;
    }
    // ... and move: the remaining horizontal distance this frame, capped
    {
        const EXVector &pos = m_pItem->Position();
        float div = XApp::GameFrameRateDiv();
        EXVector d;
        d.x = (target.x - pos.x) * div;
        d.y = div * 0.0f;
        d.z = (target.z - pos.z) * div;
        d.w = d.y;
        if (sqrtf(d.x * d.x + d.z * d.z) > limit) {
            float l = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z + d.w * d.w);
            if (l > 1e-5f) {
                float k = 1.0f / l;
                d.x *= k; d.y *= k; d.z *= k; d.w *= k;
            }
            d.x *= limit; d.y *= limit; d.z *= limit; d.w *= limit;
        }
        m_Speed = d;
        m_Unk2b8 = d;
        m_Unk2b8.w = 0.0f;
        m_Unk2b8.y = 0.0f;
    }
    pPhys->m_DeltaMove.x = 0.0f;
    pPhys->m_DeltaMove.z = 0.0f;
    pPhys->m_DeltaMove.w = 0.0f;
}

// DoHit (move mode 6, a melee attack): modern movement keeps the momentum the attack started with and
// bleeds it off over about a third of a second, instead of zeroing the speed every frame.
// MOD 00167fa0
void XItemHandler_Player::DoHit()
{
    signed char bHeavy = 0;
    signed char bChain = 0;
    if (cjmod_move_modern()) {
        // modern: an attack keeps the run's momentum and bleeds it off (CJ_ATTACK_CARRY) instead of stopping dead
        float k = cjmod_attack_carry(XApp::GameFrameRateMul());
        m_Speed.x *= k;
        m_Speed.z *= k;
        m_Speed.y = 0.0f;
        m_Speed.w = 0.0f;
    } else
        m_Speed.SetZero();
    if (m_bAnimEnded) {
        m_bAnimEnded = 0;
        ResetAttack();
        if (m_StickMag > 0.0f)
            SetupMove(1, 0, -1.0f);
        else
            SetupMove(0, 0, -1.0f);
        return;
    }
    if (CheckForFall() || CheckForJump(1)) {
        ResetAttack();
        return;
    }
    if (m_ButtonsPressed & 0x80) {
        CheckForGrabMove(1);
        CheckForGroundAttack();
        CheckForFinishingMove(1);
    }
    if (m_bAttacking && m_StickMag > 0.95f) {
        Actor *pTarget = GetTargetActor();
        float a;
        if (pTarget) {
            EXVector d = ((EXPlacementItem *)pTarget->GetItem())->Position() - m_pItem->Position();
            a = (float)atan2(d.x, d.z);
        } else
            a = m_pItem->Rotation().y;
        if ((float)fabs(Vec_AngleDifference(a, m_PadAngle)) > 0.7853982f) {
            // pushing away from the target mid-combo: break off into a run
            ResetAttack();
            m_Speed.SetZero();
            SetupMove(1, 0, -1.0f);
            return;
        }
    }
    CheckChainButtonPress();
    if (!m_bAttacking) {
        CalculateHitMove(m_AttackType);
        m_bAttacking = 0;
        return;
    }
    switch (m_AttackHit) {
    case 1:
    case 3:
        break;
    default:
        m_bAttacking = 0;
        return;
    }
    CalculateHitType(m_CurrentWeapon[m_AttackHit], bHeavy, bChain);
    SetWeapon(m_AttackHit);
    signed char bSetTime = 1;
    if (bChain && ((unsigned int *)&WeaponAttackAnims)[m_CurrentWeapon[m_AttackHit] * 6 + 4]) {   // WeaponAttackAnims[w][4] (no template operator for hybridize)
        bSetTime = 0;
        if (1.5f > m_Unk224)
            m_AttackState = 5;
        else
            m_AttackState = (~(s8)m_AttackState & 1) | 4;
    } else {
        int s = m_AttackState;
        if (1.5f > m_Unk224) {
            if ((s == 0 || s == 2) && bHeavy) bHeavy = 0;
        } else {
            if ((s == 0 || s == 2) && !bHeavy) bHeavy = 1;
        }
        m_AttackState = s + 1;
        if (m_AttackState >= 4) {
            bSetTime = 0;
            int n = m_AttackState;
            do n -= 4; while (n >= 4);
            m_AttackState = n;
        }
    }
    m_AttackHit = -1;
    m_bAttacking = 0;
    if (bSetTime) m_Unk224 = bHeavy ? 2.0f : 1.0f;
    SetupMove(6, 0, -1.0f);
    if (m_AttackState > 3) m_Unk224 = 2.0f;
}

// MOD 0016b130
// DoJump at 60 fps: falling ledge grabs searched half the radius and allowed half the height above the
// hands (distances scaled as if they were per-frame steps); both are the original at 30 fps. (The jump
// boost while the button is held is already frame-rate correct: its window runs on the multiplier.)
void XItemHandler_Player::DoJump()
{
    if (0.0f >= m_Health) {
        m_DeadFallTimer -= XApp::FrameLength();
        if (0.0f > m_DeadFallTimer) {
            theApp.m_MissionState = 4;
            theApp.m_MissionTimer = 0.0f;
            return;
        }
    }
    if (m_Mode == 3 && m_bAnimEnded) {
        m_bAnimEnded = 0;
        if (m_SubMode >= 7 && m_SubMode <= 10) SetupMove(3, 10, -1.0f);
    }
    XItemPhysics *pPhys = (XItemPhysics *)m_pItem->m_pItemPhysics;
    if (m_Mode == 3) {
        if (m_SubMode == 0xc && 0.17f > pPhys->m_DeltaMove.y) SetupMove(3, 0xd, -1.0f);
        if (m_SubMode >= 7 && m_SubMode <= 10) {
            Actor *pTarget = GetTargetActor();
            EXPlacementItem *pItem = m_pItem;
            float x, z;
            if (pTarget) {
                EXVector d;
                const EXVector &tp = ((EXPlacementItem *)pTarget->GetItem())->Position();
                const EXVector &pp = pItem->Position();
                d.x = tp.x - pp.x; d.y = tp.y - pp.y; d.z = tp.z - pp.z; d.w = tp.w - pp.w;
                x = d.x; z = d.z;
            } else {
                float yaw = pItem->Rotation().y;
                x = (float)sin(yaw) * 10.0f;
                z = (float)cos(yaw) * 10.0f;
            }
            float diff = Vec_AngleDifference(pItem->Rotation().y, (float)atan2(x, z));
            EXVector rot = pItem->Rotation();
            float step = diff >= 0.16f ? 0.16f : (-0.16f >= diff ? -0.16f : diff);
            rot.y += step;
            pItem->m_rotation = rot;
            pItem->m_validMtx = 0;
        }
    }
    MoveJump();

    if (m_bJumpTarget) {
        const EXVector &pp = m_pItem->Position();
        float dx = pp.x - m_JumpTarget.x, dy = pp.y - m_JumpTarget.y, dz = pp.z - m_JumpTarget.z;
        if (0.5f > sqrtf(dx * dx + dy * dy + dz * dz)) {
            if (m_JumpTargetType == 2) {
                Actor *pActor = GetTargetActor();
                if (pActor) {
                    DeadStop(1);
                    pPhys->SetPhysicsFlagsOn(3);
                    m_SpecialActorID = pActor->m_RefID;
                    if (AS_GetPtrFromIndex(pActor->m_SkinIndex)->m_Hash == 0xd000068 ||
                        AS_GetPtrFromIndex(pActor->m_SkinIndex)->m_Hash == 0xd000179) {
                        SetupMove(0x14, 2, -1.0f);
                        EXVector a(0.0f, 0.0f, 0.0f, 0.0f), b(0.0f, 0.0f, 0.0f, 0.0f);
                        SendMsg_AttackedByPredator(pActor, 0x30, a, b, 0.0f, 0);
                    } else {
                        SetupMove(0x14, 0, -1.0f);
                    }
                    EXBaseSoundManager::GetSoundManager()->StartSound(0x1af00353, m_pItem->Position(), -1, 0);
                    return;
                }
            } else if (m_JumpTargetType == 1) {
                DeadStop(1);
                SetupMove(0x15, 0, -1.0f);
                LandedOnVehicle();
                EXBaseSoundManager::GetSoundManager()->StartSound(0x1af00353, m_pItem->Position(), -1, 0);
                return;
            }
        }
    }

    int result = -1;
    EXGrabPointTemp g;
    if (!m_HoldState && !(0.0f >= m_Health) && !(m_Unk358 > 0.0f)) {
        EXPlacementItem *pItem = m_pItem;
        pPhys = (XItemPhysics *)m_pItem->m_pItemPhysics;
        float mul = XApp::GameFrameRateMul();
        EXVector probe = pItem->Position();
        probe.y += 1.25f;
        int ok = 1;
        if (pPhys->m_DeltaMove.y > 0.0f) {
            // rising: a ledge in reach
            g.Set(probe, 1.5f, (EXGrabPointTemp::GRABFITYPE)2);
            theItemEnv.DoGrabPointToMap(g);
            if (g.m_24 != 2 || (g.m_28 & 0x100)) ok = 0;
            else result = 1;
        }
        if (ok && 0.0f > pPhys->m_DeltaMove.y) {
            // falling: perch plane, perch, then a ledge
            // MOD: the search radius is a distance, not a per-frame step. The original scaled it by
            // the frame-rate multiplier (GameFrameRateMul: 2 at 30 fps, 1 at 60), so at 60 fps it was
            // half. Never less than at 30 fps (mul 2), where this is the original.
            float r = (mul > 2.0f ? mul : 2.0f) * 2.5f;
            result = -1;
            g.Set(m_pItem->Position(), r, (EXGrabPointTemp::GRABFITYPE)2);
            theItemEnv.DoGrabPointToMap(g);
            // MOD: CheckPerchPlaneLand / CheckPerchLand inline (DoJump is their only caller): their
            // height window, mul * 0.3, is 0.6 at 30 fps and only 0.3 at 60, so perches caught at
            // 30 fps were missed at 60. The 30 fps window at any rate, as for the ledges below.
            float pw = (mul > 2.0f ? mul : 2.0f) * 0.3f;
            const EXVector &pp0 = m_pItem->Position();
            float pdx = g.m_30.x - pp0.x, pdz = g.m_30.z - pp0.z;
            if (g.m_24 == 2 && (g.m_28 & 0x100) && !(sqrtf(pdx * pdx + pdz * pdz) > 0.5f) &&
                !((float)fabs(g.m_30.y - pp0.y) > pw)) {
                result = 5;
            } else {
                g.Set(probe, r, (EXGrabPointTemp::GRABFITYPE)1);
                theItemEnv.DoGrabPointToMap(g);
                unsigned int flags = g.m_28;
                if (g.m_24 == 1)   // CJ_GRAB_LOG=1: each falling frame that finds a ledge, and why it is not caught
                    cjmod_grab_log(0, (int)flags, g.m_30.y - probe.y, sqrtf((g.m_30.x - m_pItem->Position().x) * (g.m_30.x - m_pItem->Position().x) +
                                   (g.m_30.z - m_pItem->Position().z) * (g.m_30.z - m_pItem->Position().z)), mul, probe.y);
                if (g.m_24 == 1 && !(flags & 0x20)) {
                    float qdx = g.m_30.x - m_pItem->Position().x, qdz = g.m_30.z - m_pItem->Position().z;
                    if ((flags & 0x100) && !(sqrtf(qdx * qdx + qdz * qdz) > 0.5f) &&
                        !((float)fabs(g.m_30.y - m_pItem->Position().y) > pw)) {
                        result = 5;
                    } else if (!((float)fabs(g.m_30.y - probe.y) > (mul > 2.0f ? mul : 2.0f) * 0.5f)) {
                        // MOD: the height window is also how far above the hands a ledge can be
                        // caught at the top of the jump: 1.0 at 30 fps, but scaled by the multiplier
                        // only 0.5 at 60, so a jump that peaks just under a ledge missed it.
                        EXPlacementItem *pI = m_pItem;
                        float hx = g.m_30.x - pI->Position().x, hz = g.m_30.z - pI->Position().z;
                        if (!(sqrtf(hx * hx + hz * hz) > 1.5f)) {
                            float a = (float)atan2(g.m_Normal.x, g.m_Normal.z);
                            EXVector rot = pI->Rotation();
                            float ex = pI->Position().x - g.m_EdgePos.x, ez = pI->Position().z - g.m_EdgePos.z;
                            float flip = 0.0f;
                            int fail = 0;
                            if (Vec_AngleDifference((float)atan2(ex, ez), a) < 0.0f) {
                                if (!(flags & 0x100)) fail = 1;
                                else { flip = 1.0f; a += 3.1415927f; }
                            }
                            if (!fail) {
                                float e = (float)fabs(Vec_AngleDifference(rot.y, a) + 1.5707964f);
                                if (flags & 8) { if (e > 1.3744468f) fail = 1; }
                                else if (e > 1.1780972f) fail = 1;
                            }
                            if (fail) cjmod_grab_log(1, (int)flags, Vec_AngleDifference(rot.y, a), 0.0f, mul, probe.y);
                            else if (!CheckLineSegSpace(g, a)) cjmod_grab_log(2, (int)flags, 0.0f, 0.0f, mul, probe.y);
                            else if (!HandleShimmyAngle(g.m_30, a, 0, a)) cjmod_grab_log(3, (int)flags, 0.0f, 0.0f, mul, probe.y);
                            if (!fail && CheckLineSegSpace(g, a) && HandleShimmyAngle(g.m_30, a, 0, a)) {
                                float yaw = a;
                                if (flip != 0.0f) yaw += 3.1415927f;
                                pI->m_rotation = EXVector(rot.x, yaw, rot.z, rot.w);
                                pI->m_validMtx = 0;
                                result = (flags & 8) ? 2 : ((flags & 0x100) ? 4 : 3);
                            }
                        }
                    }
                }
            }
        }
        if (result != -1) {
            // caught something: the landing
            float dy = pPhys->m_DeltaMove.y;
            pPhys->m_physicsFlags |= 3;
            if (-0.5f > dy) {
                InflictDamageFromFall(dy);
                float f = (float)fabs(dy);
                float k = f >= 1.0f ? 1.0f : (0.5f >= f ? 0.5f : f);
                ((XGamePad *)EXBaseGamePad::GetGamePad())->PlayRumble(0x54000007, k, m_PadIndex);
            }
            pPhys->m_DeltaMove.y = 0.0f;
            m_StateFlags = g.m_28;
            m_ClamberPos = g.m_30;
            m_bLedgeTarget = 0;
            ResetAttack();
            m_pAnim->DeleteStack(1);
            EXBaseSoundManager::GetSoundManager()->StartSound(0x1af0020e, m_pItem->Position(), -1, 0);
        }
    }

    switch (result) {
    case 1: {
        m_ShimmyDir = GetSpeed() + 1.0f;
        if (1.0f > m_ShimmyDir) m_ShimmyDir = 1.0f;
        if (m_ShimmyDir > 2.0f) m_ShimmyDir = 2.0f;
        DeadStop(0);
        SetupMove(0x11, 0, -1.0f);
        return;
    }
    case 2:
        DeadStop(0);
        m_ClamberSide = 1;
        m_LockHandIdx = 2;
        SetupMove(0xc, 8, -1.0f);
        return;
    case 3:
        if (m_StateFlags & 0x40) {
            DeadStop(0);
            SetupMove(0xc, 5, -1.0f);
            return;
        }
        DeadStop(0);
        if (m_Pad.z > 0.85f && !(m_StateFlags & 0x10) && !(m_StateFlags & 0x200) && CheckForClamberSpace()) {
            SetupMove(4, (GetCardRandom() & 1) ? 4 : 3, -1.0f);
            return;
        }
        SetupMove(0xc, 0, -1.0f);
        return;
    case 4:
        DeadStop(0);
        SetupMove(0xc, (m_StateFlags & 0x40) ? 5 : 0, -1.0f);
        return;
    case 5:
        DeadStop(0);
        m_ShimmyDir = 1.5f;
        SetupMove(0x12, 0, -1.0f);
        m_HangYaw = m_pItem->Rotation().y;
        m_Unk360 = 8.0f;
        m_bUnk364 = 1;
        return;
    }
    if (!m_HoldState && m_Health > 0.0f && CheckWallMove()) return;
    if (m_Unk344 > 0.0f) {
        if (0.0f >= m_Unk344 - 8.0f && (m_ButtonsPressed & 0x20)) pPhys->m_Accel.y = 0.02440000f;
        m_Unk344 -= XApp::GameFrameRateMul();
    }
    CheckForLand();
}
