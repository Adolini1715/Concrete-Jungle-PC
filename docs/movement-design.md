# Movement redesign: "Hunter's momentum" (draft for review, 2026-10-27)

Status: proposal. Nothing here is built yet beyond the dials in docs/movement-dials.md.
Direction picked: option 1 (grounded but fluid), plus: you can move while charging a jump.

## Goals
1. Momentum carries through: run into jump into landing into run, without the stop-start the
   original has (pre-jump root, landing speed reset, run-stop slide).
2. Every input answers within 2 frames at 60 fps. Wind-ups stay as animation, not as lockout.
3. The Predator reads as an acrobat: rolls, flips and vaults the data already has (the unused
   modes listed in claude/pcj-animation-system-2026-10-27.md), used where they fit.
4. The original stays one setting away (`CJ_MOVE_STYLE=classic`), and recordings stay exact
   (all of this is off in CJ_DET, like the dials).

## What the original does that fights this (decoded, docs/movement-dials.md)
| Behaviour | Where | Effect |
|---|---|---|
| Pre-jump calls DeadStop(1) | CheckForJump -> mode 0x10 (0x0900047E) | the player stops dead while winding up |
| Landing resets horizontal speed to 1.0 / 0.9 / 0.45 / 0 of target | CheckForLand 0x16a400 | momentum lost on every landing |
| Run-stop animation root motion | CheckForStop 0x160760, mode 0 sub 9/10 | ~26-frame slide ("ice rink") |
| Air control 0.015 a frame, cap 0.1875 | MoveJump 0x150960 | jumps are committed arcs |
| Jump homing onto targets | MoveJump | nice; keep it |
| Hard landing below -0.69 | CheckForLand | keep, but give a roll-out |

## Proposed moves

### 1. Sprint
- Hold the run stick fully over for 0.6 s (or click left stick) -> sprint: top speed x1.35,
  turn rate x0.7. Uses the unused UN_SPRINT_H1_STEP mode for the step-in.
- Any stop, attack or aim drops back to run.
- Energy: none (the Predator's energy is for cloak and weapons).

### 2. Charged jump you can move during
- Holding jump starts the charge (the original's power-jump) but **does not root**: DeadStop(1)
  is skipped and the run keeps going at a reduced speed (60% of target, turn rate unchanged), so you
  can line up while charging. The pre-jump animation plays on the upper body only if the anim
  system supports a partial blend (to check: set entries have a `stack` field; stack 1 may be a
  layer), else the full-body charge pose plays and the root motion is replaced by the run velocity.
- Release: jump height scales with charge as today; the horizontal speed at release carries in.
- Tap (under 6 frames): an instant hop (no wind-up) with the run speed carried.

### 3. Directional jumps with air control
- Take-off direction is the stick direction at the press (not the facing), up to 90 degrees off
  facing: side-hops use the right strafe flip (0x0900017d) and its left mirror where present.
- Air control: 0.015 -> 0.03 a frame toward the stick, cap 0.1875 -> 0.25, but never more than
  the take-off speed + 20% (so you cannot accelerate in the air from a standing jump).
- Jump homing onto platforms/ledges stays exactly as it is; air control only applies to free jumps.

### 4. Landings that flow
- Landing speed is kept (no reset) if the stick points within 60 degrees of the travel direction;
  otherwise the original reset applies (you meant to stop).
- Running landing at speed: UN_Land_To_Run_Quick (unused mode) instead of the stand landing.
- Hard landing (below -0.69): press jump or roll within 6 frames of touch-down -> land roll
  (UN_Run_Jump_Land_Roll_Run, unused mode), no fall damage, keep 80% speed. Missed: original
  hard landing and damage.

### 5. Running roll (dodge)
- Crouch while running -> roll in the stick direction: UN_Run_H2_ROLL_LEFT/RIGHT
  (0x09000181/0x09000180) for side rolls, forward roll from the land-roll clip.
- 18-frame roll, invulnerable frames 3-10 (to tune), keeps run speed, 30-frame cooldown.
- Standing crouch stays crouch.

### 6. Stops
- The run-stop slide becomes optional: CJ_SNAPPY_STOP already exists. Proposed default: a short
  stop (keep the first 8 frames of the run-stop animation, then idle), so it still reads as
  weight without the slide.

### 7. Mantles and ledges that keep momentum
- When a jump homes onto a ledge at run speed, a clamber (mode 4) is replaced by a vault if the
  ledge is under 1.2 units above the take-off: no hang, speed kept, run continues on top.
  Clip: candidate UN_RUN_JUMP_Up (unused); to confirm by playing it with CJ_ANIM_TEST.
- Wall jump (mode 0xa): keep, add the wall-cling flip (0x09000089) as the off-wall animation.

### 8. Forward flip
- Jump + forward while sprinting -> forward flip (0x09000146, verified to play): same arc as a
  running jump +15% distance. Pure style; cheap once the sprint exists.

## Build order (each step: build, CJ_ANIM_TEST / scripted route, A/B frames, a recording that
still replays with classic style)
1. Moving charge (the one you asked for): skip DeadStop(1) in the pre-jump, keep run velocity.
   Needs: DoPreJump and CheckForJump native (pcj-decomp), ~2 functions.
2. Landing keeps speed + quick land-to-run.
3. Air control and directional take-off.
4. Land roll on hard landings.
5. Running roll dodge.
6. Sprint and forward flip.
7. Vault mantles (needs the most new code: ledge height test against the jump target).

## Direction (2026-10-27)
Yautja are large but agile and acrobatic: weight, never sluggish. No land roll (he won't use it).

## Built (2026-10-27, CJ_MOVE_STYLE=modern, default)
- Charge a jump while moving at 60% (CheckForJump, DoCrouch mods); the jump keeps the momentum.
- Quick landing: hard landings below CJ_QUICK_LAND_MAX (1.3/frame) play UN_Land_To_Run_Quick
  (17 frames) instead of the 60-frame recovery; full fall damage.
- Base movement ("agile, not sluggish"), all modern-only, each a pcj.ini dial:
  - Short run stop: the run-stop animation is cut to idle after CJ_STOP_LENGTH (30%) of it
    (DoIdle sub 9/10); no ice-rink slide.
  - No skid: reversing more than 100 degrees at speed pivots (turn x4 for that frame, speed
    halved) instead of the skid mode 0xe (MoveNormal).
  - Turning x1.85, and turning costs speed at half the original rate (MoveNormal).
  - Smooth speed curve: ground speed = 0.09375 * stick^CJ_SPEED_CURVE (2.2) instead of the
    walk/jog/run three-piece curve whose last 10% of stick doubles the speed. Applied by remapping
    the run target (cjmod_run_remap) so InertiaToRunSpeed stays the original: replacing it in C
    broke recordings, because the original's x87 result feeds the movement unrounded.
  - Air control x2.5 (CJ_AIR_CONTROL 250, MoveJump's 0.015 per-frame push).
- Turn-and-flip (2026-10-29): tried (a jump after reversing the stick while running played the lock-on forward flip) and removed 2026-10-30; it did not feel right in play.

## Decisions (2026-10-27)
- Charging a jump: move at 60% of the run target speed.
- Fall damage stays.
- There is no stealth crouch: mode 2 is the jump charge (DoCrouch 0x1689c0). Holding jump while
  standing crouches and zeroes speed; release after 0.1 s / 1/3 s jumps to 6.5 / 10 units
  (when unlocked), else 3.2. The 60% move applies here and to the running pre-jump (mode 0x10).
- Still open: roll button (crouch is not a button, so it needs its own: proposal R3 / a key),
  default stop (proposal: short stop), how much the land roll forgives (proposal: half damage).

## What has to be decompiled first
CheckForJump, DoPreJump, the crouch/roll entry (DoCrouch), CheckForClamber; MoveStrafe and
XItemPhysics::DoCollisions are in progress (overnight task) since every move above runs through
them.
