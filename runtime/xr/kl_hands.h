// Hand tracking for OVRPlugin guests: ARKit's hand joints turned into the
// 24-bone hand skeleton OVRPlugin reports (ovrp_GetHandState / ovrp_GetSkeleton).
//
// This file is only the arithmetic. kl_ovrp.c owns the seam from the frontend
// (kl_ovrp_set_hand_joints), the answers to the guest and the switch that turns
// it on; KleptonControllers.swift owns ARKit. tests/t_hands.c runs this on the
// Mac against synthetic hands.
//
// THE OVR CONVENTION. Read off the hand skeleton the plugin hands out on a Quest
// (the bind pose Meta's Interaction SDK bakes for editor use; its numbers are
// not copied here), in the plugin's own space (right-handed, +Y up, -Z
// forward):
//
//   left hand:  +X runs along the bone toward the fingertip, +Y out of the
//               palm (palmar), +Z toward the little-finger side
//   right hand: +X runs along the bone toward the WRIST, +Y out of the back of
//               the hand (dorsal), +Z toward the thumb side
//
// The same holds for the root (wrist) pose. The right hand is the left hand
// mirrored, and with these frames a mirror leaves every local bone ROTATION
// unchanged and negates every local bone POSITION — which is exactly the
// relation between the two hands in Meta's table, and what tests/t_hands.c
// checks for this construction.
//
// WHAT COMES FROM WHERE. A bone's X axis is built from joint POSITIONS (the
// direction to the next joint), so it is exact whatever axes ARKit's joint
// transforms use. Only the twist about the bone needs ARKit's joint
// orientation, and which of its axes points out of the back of the finger is
// not documented: it is found at run time (kl_hands_ctx.cal) on a flat hand,
// by comparing every joint axis with the palm's normal, and logged. Until then
// the palm's normal stands in.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ARKit's joints, in HandSkeleton.JointName order. The frontend fills an array
// in THIS order by name, so a reordered Swift enum cannot shift it.
enum {
    KLH_WRIST = 0,
    KLH_THUMB_KNUCKLE, KLH_THUMB_IB, KLH_THUMB_IT, KLH_THUMB_TIP,
    KLH_INDEX_META, KLH_INDEX_KNUCKLE, KLH_INDEX_IB, KLH_INDEX_IT, KLH_INDEX_TIP,
    KLH_MIDDLE_META, KLH_MIDDLE_KNUCKLE, KLH_MIDDLE_IB, KLH_MIDDLE_IT, KLH_MIDDLE_TIP,
    KLH_RING_META, KLH_RING_KNUCKLE, KLH_RING_IB, KLH_RING_IT, KLH_RING_TIP,
    KLH_LITTLE_META, KLH_LITTLE_KNUCKLE, KLH_LITTLE_IB, KLH_LITTLE_IT, KLH_LITTLE_TIP,
    KLH_FOREARM_WRIST, KLH_FOREARM_ARM,
    KLH_JOINTS                                  // 27
};

// OVRPlugin.BoneId for the hand skeleton (OVRP_1_44_0), and its limits.
enum {
    KLO_WRIST = 0, KLO_FOREARM, KLO_THUMB0, KLO_THUMB1, KLO_THUMB2, KLO_THUMB3,
    KLO_INDEX1, KLO_INDEX2, KLO_INDEX3, KLO_MIDDLE1, KLO_MIDDLE2, KLO_MIDDLE3,
    KLO_RING1, KLO_RING2, KLO_RING3, KLO_PINKY0, KLO_PINKY1, KLO_PINKY2, KLO_PINKY3,
    KLO_THUMB_TIP, KLO_INDEX_TIP, KLO_MIDDLE_TIP, KLO_RING_TIP, KLO_PINKY_TIP,
    KLO_BONES,                                  // 24 (SkeletonConstants.MaxHandBones)
    KLO_CAPSULES = 19,                          // SkeletonConstants.MaxBoneCapsules
};

typedef struct { float p[3]; float q[4]; } kl_hand_xf;       // q = x, y, z, w

typedef struct {
    int16_t parent;                             // -1 for the wrist
    kl_hand_xf pose;                            // relative to the parent bone
} kl_ovr_bone;

typedef struct {
    int16_t bone;
    float a[3], b[3], radius;                   // in that bone's frame
} kl_ovr_capsule;

typedef struct {
    kl_ovr_bone bone[KLO_BONES];
    kl_ovr_capsule cap[KLO_CAPSULES];
} kl_ovr_skel;

// The bind pose this runtime reports: a generic adult hand of its own (not
// Meta's numbers), the right hand the mirror of the left.
void kl_hands_bind(int hand, kl_ovr_skel *out);

// One hand's state, OVR's shape (HandStateInternal without the timestamps).
typedef struct {
    int tracked;                // the root pose and bones are this frame's
    int high_confidence;
    int finger_high[5];         // thumb, index, middle, ring, pinky
    kl_hand_xf root;            // the wrist, in the space the joints came in
    float rot[KLO_BONES][4];    // local to the parent bone; [0] and [1] identity
    float pinch[5];             // 0..1
    uint32_t pinch_bits;        // HandFingerPinch: 1 << finger
    float scale;                // this hand against the bind pose
} kl_ovr_hand;

// Per hand, kept across frames by the caller (kl_hands_init once).
typedef struct {
    // Which signed axis of ARKit's joint frame points out of the back of the
    // finger: 0..5 = +x -x +y -y +z -z, -1 while unknown.
    int dorsal_axis, distal_axis;
    float dorsal_score, distal_score, thumb_dot;
    int flat_frames;
    double acc_dorsal[6], acc_distal[6], acc_thumb[6];
    int just_latched;           // set for one call when the axes were found
    float scale;
    uint32_t pinch_bits;
    uint8_t pinch_low[5];       // frames each finger has been below the release level
    uint8_t fist_on[4];         // fist assist engaged per finger (index..little)
} kl_hands_ctx;

void kl_hands_init(kl_hands_ctx *ctx);

// Joints in one space (the caller's: ARKit world with the app's rebase).
// `tracked` has bit j set for every joint ARKit tracks this frame. Returns
// out->tracked.
int kl_hands_to_ovr(int hand, const kl_hand_xf joints[KLH_JOINTS], uint32_t tracked,
                    kl_hands_ctx *ctx, kl_ovr_hand *out);

// The pointer ray OVR reports with a hand: from the web between thumb and index
// finger, pointing away from `shoulder` (the caller's estimate), its up as close
// to world +Y as that allows. -Z forward, as every OVR pose.
void kl_hands_pointer(const kl_hand_xf joints[KLH_JOINTS], const float shoulder[3],
                      kl_hand_xf *out);

// Forces the ARKit dorsal axis instead of finding it (0..5, as above).
void kl_hands_force_axis(kl_hands_ctx *ctx, int dorsal_axis);

// A made-up hand for a run without ARKit (KL_OVRP_FAKE_HANDS): the bind pose,
// palm down at `pos` with the fingers along `fwd`, every finger bent by
// `curl_deg` at each joint. Its joint axes are OVR's.
void kl_hands_synth(int hand, const float pos[3], const float fwd[3], float curl_deg,
                    kl_hand_xf joints[KLH_JOINTS]);

// Each finger bone's turn away from the bind pose (index to little finger, not
// the thumb) times `gain`, at most `cap_deg` per joint: a normal fist read as the
// tight one a game's grab expects.
void kl_hands_scale_bend(int hand, kl_ovr_hand *o, float gain, float cap_deg);

// A loose fist read as a fist: a finger (index to little) bent more than
// `from_deg` in all (its three joints toward the palm) is bent further, so that
// `at_deg` becomes `to_deg`; fingers below `from_deg` are left as they are.
// Engaged above `from_deg`, it stays engaged down to `from_deg` - 30 (state in
// `ctx`, which may be NULL for no memory).
void kl_hands_fist_assist(int hand, kl_hands_ctx *ctx, kl_ovr_hand *o, float from_deg,
                          float to_deg, float at_deg);

const char *kl_hands_axis_name(int axis);       // "+x" .. "-z", "?"
const char *kl_hands_bone_name(int bone);

#ifdef __cplusplus
}
#endif
