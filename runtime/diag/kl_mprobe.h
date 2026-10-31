// Managed-side probe: ask Unity what it thinks, in Unity's own words.
//
// Every input measurement so far has been taken on the *native* side of the
// boundary — what ovrp handed libunity, what libunity's joystick fill wrote.
// That leaves the last hop unmeasured: whether any of it survives into
// `UnityEngine.Input`, which is where this title actually reads controllers
//  (the InputManager binds TriggerLeftHand/TriggerRightHand to joystick axes 8 and 9). "Where Unity learns how many joysticks exist" was
// the named gap; `Input.GetJoystickNames()` answers it directly.
//
// libil2cpp exports the standard embedding API, so no address table or
// hand-built calling convention is needed: domain -> assembly -> image ->
// class -> MethodInfo -> il2cpp_runtime_invoke. The runtime does class init,
// argument marshalling and boxing itself, which is why this is the robust
// way in rather than calling compiled method bodies directly.
//
// Diagnostic only, and off unless KL_PROBE_INPUT is set.
#ifndef KL_MPROBE_H
#define KL_MPROBE_H

// Called from the frame pump with the frame index. Prints a line every
// KL_PROBE_EVERY frames (default 120). No-op unless KL_PROBE_INPUT=1.
void kl_mprobe_tick(unsigned frame);

// AC Nexus VR only, self-gated on the target: take out the fixed frame length
// the game spins itself out to for SpaceWarp. See kl_mprobe.c.
void kl_mprobe_nexus_frame_budget(unsigned frame);

// Gun Club VR only, self-gated on the target: the game's own TBMPrefs.Save,
// before Unity's teardown on quit. See kl_mprobe.c.
void kl_mprobe_gunclub_save_preferences(void);

#endif

// User-initiated visionOS selection ray, in Unity-handed head-local metres.
void kl_brink_selection_push(float ox,float oy,float oz,float dx,float dy,float dz,int hand,int kind,float mx,float my);
