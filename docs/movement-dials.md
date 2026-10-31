# Movement dials (pcj.ini)

Percent of the original; 100 (or unset) is exactly the original game. Frame-rate correct.
Not applied in deterministic mode (CJ_DET, recordings), so recordings stay exact.
Code: src/native/hybrid/cjmod_move.c, mods/Player.cpp (MoveNormal), mods/XItemPhysics.cpp.

| Setting | What it changes | Original |
|---|---|---|
| `CJ_RUN_ACCEL` | How fast the run gets up to speed. 200 = twice as fast. | each frame closes 20% of the gap to the stick's speed |
| `CJ_SNAPPY_STOP` | `1`: letting go of the stick while running fast stops on the spot. The original plays a run-stop animation (mode 0/9-10) whose root motion is most of the "ice rink" slide (~26 game frames). | off |
| `CJ_RUN_STOP` | How fast the run slows when the stick is eased off (not fully released). 300 = three times faster. | keeps 93.75% of its speed each frame |
| `CJ_TURN_RATE` | How fast the running Predator turns toward the stick. | InterpAngle rate 0.12 |
| `CJ_JUMP_HEIGHT` | Height of every player jump (normal, wall, weapon, gecko). | normal jump 3.217 units |
| `CJ_GRAVITY` | The player's gravity only. Jump height is kept (thrust is computed from gravity), so higher gravity = less hang time, a heavier jump. | 0.016333 |

Example, a weightier and more responsive Predator:

    CJ_GRAVITY=160
    CJ_SNAPPY_STOP=1
    CJ_RUN_STOP=300
    CJ_RUN_ACCEL=150

## What the original does (from the decompiled code, 2026-10-27)
- Running (XItemHandler_Player::MoveNormal 0x165450): facing eases toward the stick
  (InterpAngle 0.12), the body banks up to pi/8; speed eases as above; a reversal of more than
  100 degrees within 3 updates while moving starts a skid (mode 0xe).
- In the air (MoveJump 0x150960): with a jump target (platform or ledge) the jump homes onto it
  (turns up to 5 degrees a frame, horizontal step capped at 0.25 / 0.094). Free jumps: the stick
  adds 0.015 a frame while airborne, horizontal speed capped at 0.1875.
- DoJump 0x16b130: attack jumps turn toward the target (0.16 rad a frame); every frame looks for
  something to catch (ledge in reach while rising; perch plane, perch or ledge while falling);
  a second jump press 4+ frames in (within a 12-frame window) adds upward acceleration 0.0244.
- Stopping (CheckForStop 0x160760): stick released at a target speed of 0.18 or more plays the
  run-stop animation (mode 0, submode 9 or 10); slower, straight to idle.
- Landing (CheckForLand 0x16a400): horizontal speed is reset to 1.0 / 0.9 / 0.45 / 0 by the
  run speed being aimed for; landing faster than 0.69 down is a hard landing (mode 8, recovery
  animation, fall damage).
