// Mod: frame-rate independent third-person camera easing.
// SetCamThirdPerson eases several things by a fixed amount per frame, tuned for
// the Xbox's 30 fps: the side offset (0.1 per frame), the lock-on look-at blend
// (x0.95 per frame) and the camera distance (1/7 of the gap per frame moving in,
// 1/30 moving out). At 60 fps all of them ran twice as fast, so the camera
// snapped in and out of walls and lock-on. Each step is now scaled to the time
// the frame covers (k30 = 30 Hz frames this frame); at 30 fps k30 is 1 and the
// arithmetic is the original's.
#include "EXBaseGamePad.h"
extern "C" float cjmod_powf(float x, float y);
// Modern camera (src/native/hybrid/cjmod_cam.c): in normal third person the
// native controller owns yaw and pitch (no coasting, gentle follow), the
// look-at smoothing is time-based, the distance eases out after a hold and
// the camera position is placed exactly (no second chase). CJ_CAM_MODERN=0 or
// PC Settings > Modern Camera off: the original camera.
extern "C" int cjmod_cam_step(float game_yaw, float game_pitch, float tgt_yaw, float tgt_pitch, float rx, float ry,
                              float lx, float ly, float rot, float speed, int pmode, int cmode, int inv_x, int inv_y,
                              float mul);
extern "C" float cjmod_cam_out(int i);
extern "C" float cjmod_cam_pivot_k(int axis);
extern "C" float cjmod_cam_dist(float cur, float blocked);

// MOD 000c5210
void XItemHandler_Camera::SetCamThirdPerson()
{
    XItemHandler_Player *pPlayer = XItemHandler_Player::GetInstance();
    EXBaseGamePad *pPad = EXBaseGamePad::GetGamePad();
    u8 pad = (u8)m_ViewIndex;
    int modern = cjmod_cam_step(m_Angle, m_Elevation, m_TargetAngle, m_TargetElevation,
                                pPad->Analog(pad, (EXPad::AnalogChan)2), pPad->Analog(pad, (EXPad::AnalogChan)3),
                                pPad->Analog(pad, (EXPad::AnalogChan)0), pPad->Analog(pad, (EXPad::AnalogChan)1),
                                gpPlayerItem[m_ViewIndex]->Rotation().y, pPlayer->GetSpeed(), (int)pPlayer->m_Mode,
                                (int)m_Mode, (int)m_bInvertX, (int)m_bInvertY, XApp::GameFrameRateMul());
    if (modern) {
        m_Angle = cjmod_cam_out(0);
        m_TargetAngle = m_Angle;
        m_Elevation = cjmod_cam_out(1);
        m_TargetElevation = m_Elevation;
    }
    EXVector look;
    pPlayer->GetLookAt(look);
    look.w = 0.0f;
    float base = pPlayer->GetBaseHeight();
    float h;
    switch (pPlayer->m_Mode) {
    case 2: {
        float c = pPlayer->m_CrouchTimer * 3.0f;
        float one = 1.0f;
        h = (1.88f - (c < one ? c : one) * 0.88f) + base;
        break;
    }
    case 0x11:
    case 0x12:
        h = base + 1.0f;
        break;
    case 0x14:
        h = base;
        break;
    default:
        h = base + 1.88f;
        break;
    }
    float d = 3.1415927f - m_Distance;
    float f = (float)sin((d > 0.0f ? d : 0.0f) * 0.5f) * 0.4f;
    if (m_Elevation > 1.5707964f)
        Fisher = (3.1415927f - m_Elevation) * f * 0.63661975f;
    else
        Fisher = f;
    look.y = Fisher + h;
    if (pPlayer->m_Mode == 0 && pPlayer->m_SubMode == 4) look.y = (Fisher + h) - 1.0f;

    if (m_Mode == 1) {
        Actor *pActor = Actor::GetPtrFromID(m_TargetID);
        if (pActor && pActor->m_SM.GetState() == STATE_HeavyFinish) {
            EXVector c = pActor->GetChestPos();
            look.x = (c.x + look.x) * 0.5f;
            look.y = (c.y + look.y) * 0.5f;
            look.z = (c.z + look.z) * 0.5f;
            look.w = (c.w + look.w) * 0.5f;
            m_TargetDistance = 15.0f;
        }
    }
    if (pPlayer->m_Mode == 4 && (pPlayer->m_SubMode == 10 || pPlayer->m_SubMode == 9)) {
        look = pPlayer->GetTopHeadPos();
        m_LookSmooth = 10.0f;
    }
    m_Warp = 10.0f;

    float side = 0.0f;
    if (pPlayer->m_Mode == 0xc) side = 1.0f;
    if (pPlayer->m_Mode == 4) {
        m_LookSmooth = 10.0f;
        switch (pPlayer->m_SubMode) {
        case 0:
        case 1:
            m_LookSmooth = 20.0f;
            break;
        case 5:
        case 6:
        case 7:
            side = 1.0f;
            break;
        }
    }
    float k30 = XApp::GameFrameRateMul() * 0.5f;   // 30 Hz frames this frame (1 at 30 fps)
    if (m_SideOffset > side) {
        float v = m_SideOffset - 0.1f * k30;
        m_SideOffset = side >= v ? side : v;
    }
    if (side > m_SideOffset) {
        float v = m_SideOffset + 0.1f * k30;
        m_SideOffset = v >= side ? side : v;
    }
    if (m_SideOffset != 0.0f) {
        float rot = gpPlayerItem[m_ViewIndex]->Rotation().y;
        look.x = look.x - (float)sin(rot) * m_SideOffset;
        look.z = look.z - (float)cos(rot) * m_SideOffset;
    }

    float n = m_LookSmooth / XApp::GameFrameRateMul();
    float one = 1.0f;
    float k = one / (one >= n ? one : n);
    EXVector delta;
    float kv = k;
    if (modern && pPlayer->m_Mode != 4) {
        k = cjmod_cam_pivot_k(0);
        kv = cjmod_cam_pivot_k(1);
    }
    delta.x = (look.x - m_CameraLookAt.x) * k;
    delta.y = (look.y - m_CameraLookAt.y) * kv;
    delta.z = (look.z - m_CameraLookAt.z) * k;
    delta.w = (look.w - m_CameraLookAt.w) * k;
    m_CameraLookAt.x += delta.x;
    m_CameraLookAt.y += delta.y;
    m_CameraLookAt.z += delta.z;
    m_CameraLookAt.w += delta.w;
    m_pCamera->SetTarget(m_CameraLookAt);

    if (m_Mode == 1) {
        float half = 0.5f;
        PrevInterp = PrevInterp * (k30 == 1.0f ? 0.95f : cjmod_powf(0.95f, k30));
        if (half >= PrevInterp) PrevInterp = half;
        if (PrevID != m_TargetID) PrevInterp = 1.0f;
        Actor *pActor = Actor::GetPtrFromID(m_TargetID);
        if (pActor) {
            EXVector c = pActor->GetChestPos();
            c.y -= 2.0f;
            float r = 1.0f - PrevInterp;
            EXVector t;
            t.x = m_CameraLookAt.x * PrevInterp + c.x * r;
            t.y = m_CameraLookAt.y * PrevInterp + c.y * r;
            t.z = m_CameraLookAt.z * PrevInterp + c.z * r;
            t.w = m_CameraLookAt.w * PrevInterp + c.w * r;
            m_pCamera->SetTarget(t);
        }
        PrevID = m_TargetID;
    } else {
        PrevID = 0;
    }

    if (!pPlayer->m_bScanMode) {
        float w0 = GetWhiskerDistance(0.0f, 0.0f, 0);
        float w1 = GetWhiskerDistance(0.1308997f, 0.0f, 0);
        float w2 = GetWhiskerDistance(-0.1308997f, 0.0f, 0);
        float w3 = GetWhiskerDistance(0.0f, 0.1308997f, 0);
        float w4 = GetWhiskerDistance(0.0f, -0.1308997f, 0);
        if (w1 < m_TargetDistance) m_TargetDistance = w1;
        if (w2 < m_TargetDistance) m_TargetDistance = w2;
        if (w3 < m_TargetDistance) m_TargetDistance = w3;
        if (w4 < m_TargetDistance) m_TargetDistance = w4;
        float div = 7.0f;
        if (m_TargetDistance > m_Distance) div = 30.0f;
        if (modern) {
            float blocked = m_TargetDistance;
            if (w0 < blocked) blocked = w0;
            m_Distance = cjmod_cam_dist(m_Distance, blocked);
        } else if (k30 == 1.0f)
            m_Distance = (m_TargetDistance - m_Distance) / div + m_Distance;
        else
            m_Distance = (m_TargetDistance - m_Distance) * (1.0f - cjmod_powf(1.0f - 1.0f / div, k30)) + m_Distance;
        if (m_Distance < 0.1f) m_Distance = 0.1f;
        if (w0 < m_Distance) m_Distance = w0;
    }

    float dist = m_Distance;
    float se = (float)sin(m_Elevation);
    float sa = -((float)sin(m_Angle) * se);
    float ca = -((float)cos(m_Angle) * se);
    float ce = -(float)cos(m_Elevation);
    m_CameraPosTarget.x = dist * sa + m_CameraLookAt.x;
    m_CameraPosTarget.y = m_CameraLookAt.y + dist * ce;
    m_CameraPosTarget.z = m_CameraLookAt.z + dist * ca;
    m_CameraPosTarget.w = m_CameraLookAt.w + dist * look.w;
    if (modern) m_CameraPos = m_CameraPosTarget;
}
