// Mod: player jump height dial (CJ_JUMP_HEIGHT in pcj.ini, percent; src/native/hybrid/cjmod_move.c).
// Every player jump (pre-jump, wall jump, gecko jump-off, weapon jumps) sets its thrust here
// from a target height; only the player's physics is scaled. At 100 it is the original.
extern "C" float cjmod_jump_scale(void);

// MOD 001a4390
void XItemPhysics::SetThrustToAttainHeight(float h)
{
    XItemHandler_Player *pPlayer = XItemHandler_Player::Instance();   // the unit's stand-in: m_pItem at +4
    if (pPlayer && pPlayer->m_pItem && pPlayer->m_pItem->m_pItemPhysics == this)
        h *= cjmod_jump_scale();
    h = m_Gravity * h * 2.0f;
    m_DeltaMove.y = h > 0.0f ? (float)sqrt(h) : 0.0f;
}
