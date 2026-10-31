# Enemies attacking the hidden player in cutscenes

Symptom: in Midsummer Night Massacre, the hallway cutscene after the doors
explode, the gangsters run up and stab at an empty spot in front of the
Predator puppet instead of dropping to their knees.

Cause (measured in cloud runs, tools/pos_track.py replaying a real route):
- The hallway trigger starts the cutscene and, in the same frame, an
  XItemHandler_Environment teleport (flags 0xA: no fade) moves the hidden
  player to (33.09, 16, -32.56), where play resumes. That spot is about 11 m
  in front of POIs 0x1B/0x1C, where the spawned gangsters stop.
- The gangsters (profiles 0x47000224, start state RunToLocationIgnore1)
  reach those Location1 POIs, which have no vendor state, return to
  DefaultIdle, and Actor::CheckSeePlayer sees the hidden player.
  Actor::UpdateSeeingPlayer then Gosubs the profile's sight state,
  STATE_AttackPred (0x4D000038).
- At the cutscene end the script itself ExtGotos them into STATE_AttackPred
  (Actor::UpdateFlagChannel), so attacking is meant to start then.

Fix: src/cutscene_ai.c wraps ActorOptionsManager::PostProcess. While a
cutscene runs and the player is hidden it clears
ActorOptionsManager::m_bPlayerVisible and every actor's player-seen state.
Result: the gangsters idle through the scene and charge when it ends.
CJ_CUTSCENE_HIDE=0 turns it off.

Debug tools added on the way: src/ai_trace.c (cmake -DCJ_AI_TRACE=ON,
CJ_AI_TRACE=<state VAs>|all logs StateMachine transitions with caller, player
teleports and fades), gamelog actors_near fields (state stack, locals, POI,
profile, item/head/feet positions) and flagch events, CJ_POS_TRACK and the
Killnear test action in src/quicksave.c.
