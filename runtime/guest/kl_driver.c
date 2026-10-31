// See kl_driver.h.
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "klepton.h"
#include "kl_driver.h"
#include "kl_obbmap.h"
#include "kl_tuned.h"
#include "kl_egl.h"
#include "kl_env.h"
#include "kl_guestpoke.h"
#include "kl_jkxr.h"
#include "kl_native.h"
#include "kl_sdl2.h"
#include "kl_gles3jni.h"
#include "kl_jni.h"
#include "kl_linuxproc.h"
#include "kl_mediandk.h"
#include "kl_opensl.h"
#include "kl_openxr.h"
#include "kl_ovrp.h"
#include "kl_ovrplat.h"
#include "kl_steamapi.h"
#include "kl_mprobe.h"
#include "kl_songs.h"
#include "kl_extension.h"
#include "kl_sample.h"
#include "kl_slink.h"
#include "kl_ue4.h"

typedef int    (*jni_onload_fn)(void *vm, void *reserved);
typedef int8_t (*nativeloader_load_fn)(void *env, void *clazz, void *path);

static const kl_target *g_target;
static char             g_libdir[1024];
static kl_slink_door    g_door;
static char             g_error[512];
static void           (*g_phase_hook)(const char *);
static unsigned         g_alarm = 20;
static unsigned         g_frames;
static int              g_gap_only;

// The Unity guest's handle and its resolved nativeRender, kept between _begin
// and each _frame. A NULL g_render is how _frame knows _begin has not run.
static void *g_thiz, *g_render;

static void phase(const char *p) { if (g_phase_hook) g_phase_hook(p); }

static int fail(const char *msg) {
    snprintf(g_error, sizeof g_error, "%s", msg ? msg : "(no reason given)");
    return 1;
}

// Every print goes through here so `out` may be NULL at any call site.
static void P(FILE *out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void P(FILE *out, const char *fmt, ...) {
    if (!out) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fflush(out);
}

// Per-target env defaults: bake in a title's validated flag set so it need not be
// typed on every launch. setenv with overwrite=0 means an explicit KL_* from the
// launch environment (run.sh forwards every KL_* to the device) still wins, so the
// documented per-flag A/B overrides (e.g. KL_HL2_TWOPASS_OPAQUE=0) keep working.
// Applied here, at the very top of driver init, because it must precede every reader:
// the hl2 cfg synth (kl_libc.c), ANGLE's own getenv("KL_ANGLE_MV_RELAX"), and the
// cached-static flag defaults in kl_glfb.c. The THREADED_MIRROR / CINEMA_CTX fixes
// already default-on for hl2 via their own target check, so they are not repeated.
static void kl_driver_env_defaults(const kl_target *t) {
    if (!t || !t->name) return;
    if (!strcmp(t->name, "x8016534201745429")) {
        // Space Atlas VR resizes its GLES eyes during startup; the owner sees
        // the scene confined to the lower-left corner with guest foveation.
        // Diagnostic A/B: use ordinary full-eye rendering for this title.
        setenv("KL_VRR", "0", 0);
        // The tracked right hand can click, but its cursor is invisible.
        // ES 3.2 produces sampler units 179..188 (invalid); test the ES 3.0
        // rendering path before changing the hand pose or gesture mapping.
        setenv("KL_GLES_VERSION", "3.0", 0);
        // The startup shaders use read-only StructuredBuffers despite ES 3.0.
        // On device: 1536 failed compiles (std430/readonly), stale images up
        // to hundreds of milliseconds. Use the existing lossless read path.
        setenv("KL_GLFB_SSBO_TEX", "1", 0);
        // The optional Meta audio plugin is refused on this host. Repeated
        // managed calls throw each frame (1753 in the round-trip test).
        // Use the existing no-effect implementation; Wwise still owns playback.
        setenv("KL_METAXR_AUDIO_STUB", "1", 0);
        fprintf(stderr, "  [klepton] Space Atlas VR: guest foveation disabled for image-placement test\n");
    }
    if (!strcmp(t->name, "hl2")) {
        // Validated Half-Life 2 VR configuration (2026-08-31): direct-array multiview
        // + forced eye-RT allocation, single-threaded material queue (threaded wedges
        // the guest's GL context), ANGLE multiview relaxation, two-pass brush+opaque
        // (recovers the geometry the emulated single-pass multiview silently drops),
        // force-clear, visfix, no-cull, and sv_cheats (unblocks the skybox fog cvars).
        // See the hl2-* memories for why each is needed.
        setenv("KL_HL2_MULTIVIEW",     "1", 0);
        setenv("KL_HL2_FORCERT",       "1", 0);
        setenv("KL_HL2_MAT_QUEUE",     "0", 0);
        setenv("KL_ANGLE_MV_RELAX",    "1", 0);
        setenv("KL_HL2_FORCE_CLEAR",   "1", 0);
        setenv("KL_HL2_VISFIX",        "1", 0);
        setenv("KL_HL2_TWOPASS_BRUSH", "1", 0);
        setenv("KL_HL2_TWOPASS_OPAQUE","1", 0);
        setenv("KL_HL2_NO_CULL",       "1", 0);
        setenv("KL_HL2_SV_CHEATS",     "1", 0);
        // LOWMEM confirmed on device (2026-08-31): long in-game sessions were
        // signal-9 jettisoned by the OS; mat_picmip 2 + mat_reducefillrate 1 cut
        // the footprint and the session ran meaningfully longer. KL_HL2_LOWMEM=0
        // restores full-res textures.
        setenv("KL_HL2_LOWMEM",        "1", 0);
    }
    else if (!strcmp(t->name, "hl1")) {
        // Half-Life on Xash3D / Lambda1VR. Written into userconfig.cfg on device by
        // kl_xash_write_userconfig (kl_native.c), which execs AFTER config.cfg so
        // these win over the saved/archived values:
        //   vr_smoothturn/vr_turn_angle — force smooth turning (the in-menu toggle's
        //     hitbox is offset/unclickable); vr_turn_angle < 10 selects smooth mode
        //     and is also its speed (0 would be smooth-but-motionless).
        //   vr_height_adjust — the guest reads the head in LOCAL space, which Klepton
        //     places ~1.675 m below the floor (STAGE bounds query fails, so the game
        //     never gets a floor-grounded space), so the head reads ~1.68 m too high
        //     and clips low ceilings (the intro tram roof). -1.5 m cancels most of it.
        //     The real fix is the LOCAL/STAGE reference-space math in kl_openxr.c.
        // (con_logfile was tried here to capture the engine console, but this build
        // of Xash writes no logfile on this platform — verified inert — so it's
        // dropped. The engine console is only reachable on-screen.)
        setenv("KL_XASH_CVARS",
               "vr_smoothturn 1; vr_turn_angle 5; vr_height_adjust -1.5", 0);
    }
    else if (!strcmp(t->name, "cs1")) {
        // Counter-Strike VR (CSVR) with PlayStation VR2 Sense controllers: the
        // game puts duck on A and jump on B, which on a Sense pair is cross for
        // duck and circle for jump — the wrong way round for anyone used to a
        // PlayStation pad, and what its first player here asked to have swapped.
        // Written to userconfig.cfg at each start (see hl1 above), so the two
        // buttons cannot be remapped in the game's own controls page while this
        // is in force; KL_XASH_CVARS="" in the environment switches it off.
        //
        // vr_motion_activation 0: "use" follows the weapon. The game's default (3) makes the
        // moment "use" is held – or the left arm is stretched – a hand pointer: the direction
        // sent to the server is no longer the gun's but the line from the head through the
        // left hand, and letting the arm come back sends "-use" even with the trigger held.
        // Defusing needs "use" to keep hitting the bomb for seconds; reported from the
        // headset as starting and breaking off at once, which is the first half second
        // (still the gun's direction) followed by a hand that points elsewhere. With 0 the
        // player aims the gun at the bomb, the door or the hostage and holds the trigger.
        setenv("KL_XASH_CVARS", "vr_button_a \"+jump\"; vr_button_b \"+duck\"; vr_motion_activation 0", 0);
    }
    else if (!strcmp(t->name, "alyx")) {
        // Half-Life: Alyx. The eye texture the display is offered at decides how
        // large the engine's render targets are ALLOCATED — it rounds the offer
        // up by ~1.18 and its automatic fidelity level then renders into a part
        // of that, so the offer is a memory ceiling rather than a picture size.
        // At the default cap of 3072 the first level sat at a 6.9 GB footprint
        // with 1.3 GB left and the system reporting critical memory pressure
        // (Vision Pro M5, 2026-10-06), while the fidelity level it settled on
        // was drawing 2304x1920 of the 3648x2880 it had allocated. 2800 leaves
        // room above what it draws and came to ~0.5 GB less.
        setenv("KL_OVRP_EYE_MAX", "2800", 0);
        // The display rate the engine is told is its FRAME BUDGET: its fidelity
        // controller picks the largest picture whose measured frame time fits one
        // refresh (the measurement is xrizer's, patched to be real —
        // guestbuild/xrizer-frame-timing.py). Told the panel's own 100 Hz it has
        // 10 ms, cannot make it at any size, and sits at its smallest picture.
        // Frames reach the compositor on a 10 ms grid and are reprojected from
        // there, so the engine is given a budget it can meet instead: at 60 it
        // settles where a frame takes ~15 ms and lands every other refresh, a
        // steady 50 a second. Lower buys resolution with frame rate, and the
        // other way round; Documents/lx/env.txt changes it without a build.
        setenv("KL_DISPLAY_HZ", "60", 0);
        // Two things MoltenVK does to the engine's multisampled scene pass that
        // cost it every sample of that pass, written to memory and read back,
        // each frame — and that the Vulkan side can prevent (kl_vulkan.c has
        // both stories): the pass is split in two at a barrier the engine
        // records inside it, and it is loaded and stored instead of cleared and
        // discarded because its render area is a few percent smaller than its
        // attachments. Mac, first level, 2x: 17.1 -> 14.9 ms of GPU a frame.
        setenv("KL_VK_PASS_BARRIERS", "0", 0);
        setenv("KL_VK_WIDEN_AREA", "1", 0);
        // MoltenVK without MTLHeaps. With them the engine's pooled device memory
        // is one large heap per allocation whether or not it is filled, and the
        // textures MoltenVK cannot place in a heap (every depth target) are
        // paid for a second time beside it. The session with heaps off was the
        // first to stay well under the app's memory limit: 2.5 GB of Metal
        // allocations instead of ~4.5, 5.96 GB footprint, 13-14 ms of GPU a
        // frame in a level (Vision Pro M5, 2026-10-06; not a controlled A/B).
        setenv("MVK_CONFIG_USE_MTLHEAP", "0", 0);
        // "It hangs for a moment now and then" — say what each such moment was
        // (kl_stall.c). A quarter of a second is what a player notices.
        setenv("KL_STALL_MS", "250", 0);
        // The engine compiles each pipeline at the draw that first needs it, and
        // the frame waits: 300-600 ms the first time a shader is seen on a
        // device, the stalls a player meets on entering a new place. Compiled
        // behind the frame instead (kl_vulkan.c, KL_VK_ASYNC_PIPES).
        setenv("KL_VK_ASYNC_PIPES", "1", 0);
        // The frame is published when the GPU has finished it rather than the
        // guest waiting for that at xrEndFrame (kl_openxr.c, klxr_defer_*): the
        // engine builds its next frame while this one is drawn.
        setenv("KL_VK_FRAME_ASYNC", "1", 0);
        // Better than behind the frame: compiled before the game starts. The
        // system's Metal cache keeps what it has compiled, so every pipeline in
        // a list that came with the game (Documents/klfz/*.foz, built on the
        // Mac from the player's own shader files — tools/alyx_pipelines.py) is
        // compiled while the start window is still up (kl_fossilize.cpp, and
        // prepare_graphics in the app), and the game then finds it there in a
        // few milliseconds. What the game asks for beyond the list is recorded
        // beside it and is in the next start's. Only where the app has named
        // the folder: a run on the host has no such place.
        if (getenv("KL_VK_FOZ_DIR")) {
            setenv("KL_VK_FOZ_REPLAY", "auto", 0);
            setenv("KL_VK_FOZ_REPLAY_BLOCK", "1", 0);
            setenv("KL_VK_FOZ_THREADS", "6", 0);
            setenv("KL_VK_FOZ_RECORD", "auto", 0);
            setenv("KL_VK_DEVICE_INFO", "auto", 0);
            // The game's device, not the one xrizer opens its session with.
            setenv("KL_VK_FOZ_APP", "Source2", 0);
        }
    }
    else if (!strcmp(t->name, "x9269393463181825")) {
        // Roboquest VR, Meta Store build (UE 5.6 mobile renderer, OpenXR on
        // Vulkan). Its RHI thread calls xrEndFrame, and xrEndFrame waited there
        // for the GPU to finish the frame (kl_vulkan_frame_done) before the RHI
        // thread could start on the next one. With its multisampled scene in
        // real memory (KL_VK_NO_TRANSIENT) that came to 40 of 90 frames a
        // second with the GPU about a third busy and no CPU thread above 18%
        // (on device, 2026-10-23): frames missed while nothing was at its
        // limit. Its other build (x3151760) measured the same in the hub.
        // Published by a worker once the GPU is done instead, as for Alyx above
        // (kl_openxr.c, klxr_defer_*): the next frame is built while this one
        // is drawn. KL_VK_FRAME_ASYNC=0 is the old way.
        setenv("KL_VK_FRAME_ASYNC", "1", 0);
    }
    else if (!strcmp(t->name, "x9531494193591052")) {
        // Ghost Town, from community PR #41 (sibest). Its Options buttons and
        // the right-hand hold to recenter (KleptonControllers.swift), the
        // TLSDESC relocations its Unity build carries, one audio output, the
        // per-layer colour its fades are made of, the panels it submits apart
        // from its eye-only EndFrame list, and panels it destroys taken down
        // at once (klovrp_enq_retire).
        setenv("KL_GHOSTTOWN_INPUT", "1", 0);
        setenv("KL_ELF_TLSDESC", "1", 0);
        setenv("KL_AUDIO_ONE_OUTPUT", "1", 0);
        setenv("KL_GHOSTTOWN_OVERLAY_COLOR", "1", 0);
        setenv("KL_OVRP_ENQ_OWNS_OVERLAYS", "1", 0);
    }
    else if (!strcmp(t->name, "vampire")) {
        // Vampire: The Masquerade - Justice (UE 4.27, Vulkan). Ended by the
        // system on entering the sanctuary for the first time: the footprint
        // went 2.5 -> 8 GB in six seconds with no error in the log
        // (2026-10-29). KL_TRACE_MMAP named one site in libUE4 mapping 1654
        // blocks of 4 MB — a 1280x720 RGBA video sample each — as two
        // discipline movies started: the engine copied the same decoded frame
        // again on every tick (kl_jni_ue4.c, KL_MP14_NEW_FRAMES_ONLY).
        setenv("KL_MP14_NEW_FRAMES_ONLY", "1", 0);
    }
    else if (!strcmp(t->name, "x5812519008825194")) {
        // AC Nexus: ShaderWarmupManager.PopulatePSOCache is one frame that
        // compiles every pipeline in MoltenVK and then waits for the GPU to
        // work through them. The game got in after ~90 s three times; once the
        // first level was finished the same frame took longer than the app's
        // 120 s watchdog (60 s compiling, then 60+ s at the frame-end wait
        // with the GPU still submitting), and the watchdog's SIGALRM ended
        // every start from then on (2026-10-29). A watchdog is for a frame
        // that never returns, not for a slow one.
        setenv("KL_ALARM", "600", 0);
    }
    else if (!strcmp(t->name, "wrath2")) {
        // wrath2's world never appears because the GAME thread grinds through FMOD
        // Studio bank loading (UEngine::LoadMap -> LoadBanksForMap -> loadBankFile),
        // which polls with short usleeps. At DEFAULT QoS on visionOS those sleeps
        // are coalesced to 30 Hz (33 ms), so a ~few-second bank load stretches to
        // minutes and the level never finishes loading in a session. Cap the poll
        // sleep and lift the guest threads off default QoS so their timer wakeups
        // are not coalesced — same fix class as olar's audio-thread QoS starvation.
        setenv("KL_USLEEP_CAP", "250", 0);   // microseconds; the FMOD poll slept 5-33 ms/call
        setenv("KL_GUEST_QOS",  "1",   0);   // USER_INITIATED instead of default
    }
    else if (!strcmp(t->name, "walkabout") || !strcmp(t->name, "x3634830803298285")) {
        // Demeo (x3634830803298285) is here for the same complaint, word for
        // word a session later ("es ruckelt alle 5-10 Sekunden einmal für ca
        // 500 ms", 2026-10-13): its first long session compiled 240 pipelines
        // at about 300 ms each as they were first drawn, which accounts for
        // most of the stalls in its log — and not for all of them: its owner
        // sees them standing still in a room as well, and some were the
        // compositor's own presents arriving late, with the app at 7.5 to 7.7
        // of its 8.2 GB. So the watch below is on for it too.
        //
        // "It lags every few seconds" (a Vision Pro, 2026-10-11): the display
        // keeps its 100 Hz, and the GAME's frames arrive 100-300 ms late now
        // and then. Say what each such moment was — the stall watch names the
        // threads that were busy.
        //
        // What the watch found in two sessions: the long ones (0.3-0.6 s) are
        // MoltenVK compiling a shader the first time a pipeline needs it (this
        // is a Vulkan guest). A frame that arrives one refresh late about once
        // a second is there as well, and was there still with the managed
        // probe gated (kl_mprobe.c) — not explained yet.
        setenv("KL_STALL_MS", "100", 0);
        // ...and how long its frames take against the display's period, every
        // ten seconds (the app's frame loop), while this is being looked into.
        setenv("KL_GUEST_FRAME_STATS", "10", 0);
        // So its pipelines are compiled before the game starts, as Alyx's are:
        // what a session compiled is recorded in the app's klfz folder, and
        // the next start compiles that list while the start window is still
        // up (kl_fossilize.cpp, prepare_graphics in the app). The first start
        // after installing has no list and is as before. Only where the app
        // has named the folder.
        if (getenv("KL_VK_FOZ_DIR")) {
            setenv("KL_VK_FOZ_REPLAY", "auto", 0);
            setenv("KL_VK_FOZ_REPLAY_BLOCK", "1", 0);
            setenv("KL_VK_FOZ_THREADS", "6", 0);
            setenv("KL_VK_FOZ_RECORD", "auto", 0);
            setenv("KL_VK_DEVICE_INFO", "auto", 0);
        }
    }
    else if (!strcmp(t->name, "batman")) {
        // "Small hitches while playing, the game stands still for a moment"
        // (2026-10-17). Its log of twelve minutes in the story: the picture
        // stood for 0.5 to 1.6 s eight times, and each time but two the
        // render thread was in vkCreateGraphicsPipelines — 400 pipelines,
        // 15 s in the compiler, the slowest 1.1 s, as new places and enemies
        // came into view. Metal keeps what it compiled, so a place seen once
        // is smooth the next time; a story game is mostly places seen once.
        // The other two were enemies coming in with a dozen sound banks
        // loaded at once. The sound itself ran without a gap.
        //
        // As for Walkabout and Demeo: the stall watch names what each gap
        // was, the frame statistics say how the frames went, and a session's
        // pipelines are recorded and compiled before the next start (after a
        // system update has emptied Metal's cache, too). Compiling behind the
        // frame (KL_VK_ASYNC_PIPES with KL_VK_ASYNC_RP) is a test switch, on
        // from the start for this game (targets.py "test_on"): with it the
        // owner's next session had no such freeze left (2026-10-17).
        setenv("KL_STALL_MS", "100", 0);
        setenv("KL_GUEST_FRAME_STATS", "10", 0);
        if (getenv("KL_VK_FOZ_DIR")) {
            setenv("KL_VK_FOZ_REPLAY", "auto", 0);
            setenv("KL_VK_FOZ_REPLAY_BLOCK", "1", 0);
            setenv("KL_VK_FOZ_THREADS", "6", 0);
            setenv("KL_VK_FOZ_RECORD", "auto", 0);
            setenv("KL_VK_DEVICE_INFO", "auto", 0);
        }
    }
    else if (!strcmp(t->name, "moss") || !strcmp(t->name, "x4395292760584049")) {
        // "Some shader stutters, in the Steam and in the Meta version"
        // (2026-10-17). The first session of a fresh Steam copy compiled 162
        // pipelines in four minutes, 0.3-0.7 s each, at the draw that first
        // needed them; the second start compiled none (Metal's cache), so it
        // is the places not seen yet on this installation. As for Arkham
        // Shadow: what each gap was, how the frames went, and a session's
        // pipelines recorded and compiled before the next start. Compiling
        // behind the frame (KL_VK_ASYNC_PIPES with KL_VK_ASYNC_RP) is a test
        // switch, on from the start for this game (targets.py "test_on"):
        // with it the owner's next session had no compile freeze left.
        setenv("KL_STALL_MS", "100", 0);
        setenv("KL_GUEST_FRAME_STATS", "10", 0);
        if (getenv("KL_VK_FOZ_DIR")) {
            setenv("KL_VK_FOZ_REPLAY", "auto", 0);
            setenv("KL_VK_FOZ_REPLAY_BLOCK", "1", 0);
            setenv("KL_VK_FOZ_THREADS", "6", 0);
            setenv("KL_VK_FOZ_RECORD", "auto", 0);
            setenv("KL_VK_DEVICE_INFO", "auto", 0);
        }
    }
    else if (!strcmp(t->name, "x5269080709783454")) {
        // Max Mustard (Unity 2022.3 + FMOD Studio over OpenSL). FMOD produced
        // sound in only a small fraction of its buffers — heard as near-silence
        // with the odd quiet stutter — while the banks loaded fine. wrath2's fix
        // class: at default QoS visionOS coalesces the mixer's short timed waits
        // to ~33 ms, longer than one 512-frame block at 48 kHz (10.7 ms), so the
        // output starves between blocks.
        setenv("KL_USLEEP_CAP", "250", 0);
        setenv("KL_GUEST_QOS",  "1",   0);
    }
    else if (!strcmp(t->name, "x9386827918099701")) {
        // BattleGroupVR2 0.251 (Unity 6.2, Vulkan; fix 38, 2026-10-13). The
        // app was ended at its per-process memory limit while the game was
        // still starting. Three settings are the sender's answer, each seen by
        // the sender on a Vision Pro and by nobody here; a fourth, the 2x cap
        // on multisampling, is beside its siblings in klvk_sample_mask.
        //
        // MoltenVK without MTLHeaps, as for Alyx above and for the same two
        // reasons: a pool is one heap of its full size whether or not it is
        // filled, and every depth target is paid for a second time beside it.
        setenv("MVK_CONFIG_USE_MTLHEAP", "0", 0);
        // The game is told memory is short while 1.5 GiB are still free
        // instead of 384 MiB (kl_mem_pressure_poll): what it asks for at its
        // start came in steps larger than the smaller reserve, so the warning
        // arrived after the allocation that ended the app. It reaches the game
        // through Unity 6.2's own callback (kl_jni_low_memory).
        setenv("KL_LOWMEM_FREE_MB", "1536", 0);
    }
    else if (!strcmp(t->name, "x8387152738076399")) {
        // Banners & Bastions 1.0.5 (com.dropout.island; Unity 6000.3, Vulkan,
        // Unity's OpenXR plugin with Meta's MetaXRFeature, the Interaction SDK
        // and MR Utility Kit): a mixed-reality tabletop game. First run here
        // (2026-10-17): it started, but the picture stayed glued to the head,
        // no controllers showed, the room was black, and it stuttered. The
        // same stack as DreamSpace (x9449316705178989), and the same answers.
        //
        // OVRPlugin's version. Told 1.60.0, MetaXRFeature never reached the
        // native OnInstanceCreate ("[MetaXRFeature] OnInstanceCreate returned
        // an error") and the Meta half of the SDK stayed off: its camera rig,
        // its controllers, its passthrough (kl_ovrp.c, klovrp_version_str). It
        // is told the version its own libOVRPlugin.so carries.
        setenv("KL_OVRP_VERSION", "1.201.0", 0);
        // ...and, as for every title told a plugin version this far above the
        // 1.60.0 these answers were collected against, an entry point with no
        // answer says UNSUPPORTED and is named once instead of ending the game
        // (kl_ovrp.c, KL_OVRP_UNANSWERED). Its first was
        // ovrp_SetControllerDrivenHandPoses, right after the session began:
        // hands drawn from controllers are not something this runtime does.
        setenv("KL_OVRP_UNANSWERED", "unsupported", 0);
        // The room around the board: passthrough is offered, as to the other
        // mixed-reality titles (kl_ovrp.c, klovrp_pt_on), and the app keeps the
        // mixed space for it (KleptonTuning.swift, targetIsMixedReality).
        setenv("KL_OVRP_PASSTHROUGH", "1", 0);
        // Meta's MR Utility Kit (libmrutilitykitshared) carries TLSDESC
        // relocations; refused, a NullReferenceException followed at start
        // (kl_image.c). Loaded, it asks for a room, which is the emulated one
        // DreamSpace gets (KL_OVRP_SCENE_ROOM, kl_ovrp.c and kl_openxr.c), and
        // LOCAL_FLOOR is answered as for DreamSpace (kl_openxr.c), which on
        // this stack asks for it once the Meta half is up.
        setenv("KL_ELF_TLSDESC", "1", 0);
        setenv("KL_OVRP_SCENE_ROOM", "1", 0);
        setenv("KL_XR_LOCAL_FLOOR", "1", 0);
        // The stutter: 145 pipelines compiled at the draw that first needed
        // each, about 300 ms apiece, in a 20-second first session. Recorded,
        // and compiled while the start window is up from the next start on,
        // as for Demeo above. Only where the app has named the folder.
        if (getenv("KL_VK_FOZ_DIR")) {
            setenv("KL_VK_FOZ_REPLAY", "auto", 0);
            setenv("KL_VK_FOZ_REPLAY_BLOCK", "1", 0);
            setenv("KL_VK_FOZ_THREADS", "6", 0);
            setenv("KL_VK_FOZ_RECORD", "auto", 0);
            setenv("KL_VK_DEVICE_INFO", "auto", 0);
        }
    }
    if (!strcmp(t->name, "x3231001163584154")) {
        // Warhammer 40,000: Battle Sister: its lightmaps are HDR ASTC (Unity's
        // GraphicsFormat 145), which Unity takes as supported only when the
        // extension string names GL_KHR_texture_compression_astc_hdr — named
        // here only if ANGLE names it (kl_egl.c, the backend candidates).
        setenv("KL_GL_BACKEND_EXT", "1", 0);
        // Its BLAST shaders are rewritten at glShaderSource (kl_glfb.c,
        // KL_GLFB_PROBE_CUBE_ARRAY, KL_GLFB_NO_GRADING_LUT); a program Unity
        // cached as a binary skips that, and keeps whatever an earlier
        // toolchain made of it. Always from source, so a change reaches them.
        setenv("KL_GLFB_PROGRAM_BINARY", "0", 0);
    }
    // Every Unity title under the profile for untested games gets what
    // Walkabout, Demeo, Arkham Shadow and Moss were given for their hitches:
    // the stall watch names what each gap was, the frame statistics say how
    // the frames went, and a session's pipelines are recorded and compiled
    // before the next start. LEGO Bricktales: "the picture stops for a moment
    // now and then" (2026-10-18). The KL_VK_* lines only act on a Vulkan guest.
    {
        extern int kl_untested_profile(void);
        if (t->kind == KL_GUEST_UNITY && kl_untested_profile()) {
            setenv("KL_STALL_MS", "100", 0);
            setenv("KL_GUEST_FRAME_STATS", "10", 0);
            if (getenv("KL_VK_FOZ_DIR")) {
                setenv("KL_VK_FOZ_REPLAY", "auto", 0);
                setenv("KL_VK_FOZ_REPLAY_BLOCK", "1", 0);
                setenv("KL_VK_FOZ_THREADS", "6", 0);
                setenv("KL_VK_FOZ_RECORD", "auto", 0);
                setenv("KL_VK_DEVICE_INFO", "auto", 0);
            }
        }
    }
}

void kl_driver_init(const kl_target *t, const char *libdir, kl_slink_door door) {
    g_target = t;
    g_door   = door;
    g_error[0] = 0;
    snprintf(g_libdir, sizeof g_libdir, "%s", libdir ? libdir : "");
    kl_driver_env_defaults(t);
    if (t && !strcmp(t->name, "x2249318828413747")) {
        // Device log: ANGLE supplies ES 3.0, while claiming 3.2 makes Unity
        // select compute skinning. Compute shader creation then fails and
        // kernels remain invalid. Describe the backend this title actually
        // has so Unity can select its supported skinning/rendering path.
        kl_egl_set_gles_version(3, 0);
        fprintf(stderr, "  [gl] Space Channel: GLES 3.0 capability profile (ANGLE backend)\n");
    }
    if (t && !strcmp(t->name, "x9449316705178989")) {
        // DreamSpace: the same story. Told 3.2, Unity skinned on the GPU, every
        // Internal-Skinning kernel was invalid (~160k log lines a session) and a
        // vertex shader reading a storage buffer failed to compile on ES 3.0 — a
        // theme's tree showed its leaves and no trunk (2026-10-16). Told 3.0,
        // Unity takes the paths this backend has.
        kl_egl_set_gles_version(3, 0);
        fprintf(stderr, "  [gl] DreamSpace: GLES 3.0 capability profile (ANGLE backend)\n");
    }
    if (t && !strcmp(t->name, "x3882524545155280")) {
        // Catan VR (Unity 2018.4, which has no disable-gpu-skinning boot
        // switch): told 3.2, it skinned the player's hands with a compute
        // shader, glCreateShader(COMPUTE) was refused (shader 0, "GLSL
        // compilation failed, no infolog provided" x12) and only the wrist
        // cuffs were drawn (2026-10-17).
        kl_egl_set_gles_version(3, 0);
        fprintf(stderr, "  [gl] Catan VR: GLES 3.0 capability profile (ANGLE backend)\n");
    }
    if (t && !strcmp(t->name, "x3919415921458774")) {
        // Gods of Gravity (Unity 2021.3.45f2): told 3.2, its wand models came
        // out as large flat surfaces of a different colour each run, from the
        // right hand out across the scene (headset, 2026-10-19/20; not the
        // pins, VFX Graph, base-vertex draws, CPU skinning or non-finite
        // positions, each switched off on its own). Told 3.0, the Mac run draws
        // the wands that were missing there under 3.2.
        kl_egl_set_gles_version(3, 0);
        fprintf(stderr, "  [gl] Gods of Gravity: GLES 3.0 capability profile (ANGLE backend)\n");
    }
    // KL_GLES_VERSION=<major>.<minor> — the GLES level described to any guest,
    // for trying a title on the 3.0 profile above without a build.
    {
        const char *v = getenv("KL_GLES_VERSION");
        int ma = 0, mi = 0;
        if (v && sscanf(v, "%d.%d", &ma, &mi) == 2 && ma >= 2 && ma <= 3 && mi >= 0 && mi <= 2) {
            kl_egl_set_gles_version(ma, mi);
            fprintf(stderr, "  [gl] KL_GLES_VERSION: GLES %d.%d described\n", ma, mi);
        }
    }

}

kl_guest_kind kl_driver_kind(void) {
    return g_target ? g_target->kind : KL_GUEST_UNITY;
}

const char      *kl_driver_error(void)  { return g_error; }
const char      *kl_driver_target_name(void) { return g_target ? g_target->name : NULL; }
// Moss and Moss: Book II (x4395292760584049): one studio, one engine (UE 4.27.2,
// Polyarc), the same device profiles and the same picture. The settings Moss was
// tuned to on the headset hold for both; the Steam build's own ones stay Moss's.
int kl_driver_target_is_moss(void) {
    const char *t = g_target ? g_target->name : NULL;
    return t && (!strcmp(t, "moss") || !strcmp(t, "x4395292760584049"));
}

// ---- The profile for untested games --------------------------------------
//
// A store title nobody here has looked at runs as a generic target: it gets
// its engine's defaults and none of the per-title answers, and everything the
// runtime does not know ends the run by name. That is right while a title is
// being brought up, and it is why most untested titles stop at start. Of the
// first 52 fixes users sent in, about twenty were a Java method with no answer,
// about fifteen an OVRPlugin entry point or import with none, nine a missing
// logged-in user, six an OVRPlugin version too old for the game's own C#, six
// Unity samplers on the wrong texture units, and several GPU skinning or a
// TLSDESC relocation.
//
// So a store title (x<app id>) that is not in kl_tuned.h gets better defaults:
// unknown OVRPlugin entry points answer UNSUPPORTED (klovrp_soft_unanswered),
// unknown Java methods and fields answer zero (klj_soft_unknown), OVRPlugin
// reports the version the game ships (KL_OVRP_GUEST_VERSION, read from its
// libOVRPlugin.so when the app is built), an offline player and an empty
// purchase list (kl_ovrplat.c), TLSDESC relocations (kl_image.c), and for a
// Unity guest on GLES its uniform pins bound at link (KL_GLFB_BIND_PINS) and
// skinning on the CPU (klj_boot_config_extra). Each of those keeps its own
// switch, which still wins. KL_UNTESTED=0 turns the profile off, =1 on for any
// target. A title in kl_tuned.h keeps exactly what it was tested with.
static int kl_target_is_store_id(const char *t) {
    if (!t || t[0] != 'x' || !t[1]) return 0;
    for (const char *p = t + 1; *p; p++) if (*p < '0' || *p > '9') return 0;
    return 1;
}
static int kl_tuned_cmp(const void *key, const void *el) {
    return strcmp((const char *)key, *(const char *const *)el);
}
int kl_target_tuned(const char *t) {
    return t && bsearch(t, kl_tuned_targets, sizeof kl_tuned_targets / sizeof kl_tuned_targets[0],
                        sizeof kl_tuned_targets[0], kl_tuned_cmp) != NULL;
}
// The same answer for a named target, for the start window, which asks before
// the driver has one.
int kl_untested_target(const char *t) {
    return kl_env_on("KL_UNTESTED", kl_target_is_store_id(t) && !kl_target_tuned(t));
}
int kl_untested_profile(void) {
    static int v = -1;
    if (v >= 0) return v;
    const char *t = kl_driver_target_name();
    if (!t && !getenv("KL_UNTESTED")) return 0;   // asked before the target is known: not cached
    v = kl_untested_target(t);
    if (v)
        fprintf(stderr, "  [klepton] %s: no title here has been tested — the profile for untested "
                        "games is on (KL_UNTESTED): soft answers for unknown OVRPlugin and Java "
                        "calls, the game's own OVRPlugin version, an offline player, TLSDESC, and "
                        "for Unity on GLES its uniform pins and skinning on the CPU\n",
                t ? t : "?");
    return v;
}
int              kl_driver_gap_only(void) { return g_gap_only; }
unsigned         kl_driver_frames(void) { return g_frames; }
void             kl_driver_note_frame(void) { g_frames++; }
void             kl_driver_set_phase_hook(void (*fn)(const char *)) { g_phase_hook = fn; }
void             kl_driver_set_alarm(unsigned s) { g_alarm = s; }

int kl_driver_owns_frame_loop(void) {
    switch (kl_driver_kind()) {
    case KL_GUEST_STEAMLINK:
    case KL_GUEST_UE4:
    case KL_GUEST_JKXR:
    case KL_GUEST_NATIVE:
    case KL_GUEST_SDL2:
    case KL_GUEST_LINUX:
    case KL_GUEST_GLES3JNI: return 1;
    default:            return 0;
    }
}

// ---- boot ------------------------------------------------------------------

static int boot_unity(FILE *out) {
    char path[1200];
    snprintf(path, sizeof path, "%s/libmain.so", g_libdir);

    phase("libmain");
    P(out, "=== libmain.so entry ===\n");
    kl_image *img = kl_load_auto(path);
    if (!img) return fail(kl_error());
    kl_register_image("libmain.so", img);
    kl_run_init(img);

    jni_onload_fn onload = (jni_onload_fn)kl_sym(img, "JNI_OnLoad");
    if (!onload) return fail("libmain.so exports no JNI_OnLoad");

    kl_jni_local_frame_push();          // the JVM would pop each native's local
    int version = onload(kl_jni_vm(), NULL);   // frame on return; the host plays
    kl_jni_local_frame_pop();           // that half (see kl_jni.h)
    P(out, "  JNI_OnLoad returned 0x%08x\n", version);
    if (version != KL_JNI_VERSION_1_6)
        return fail("JNI_OnLoad did not return JNI_VERSION_1_6");

    // libmain registers com.unity3d.player.NativeLoader.{load,unload} — the shim
    // Unity's Java side calls to dlopen libunity.so.
    const char *CLS = "com/unity3d/player/NativeLoader";
    void *load = kl_jni_native(CLS, "load", NULL);
    if (!load || !kl_jni_native(CLS, "unload", NULL))
        return fail("NativeLoader natives were not registered");
    P(out, "  registered %s.load=%p\n", CLS, load);
    P(out, "\n=== EXIT CRITERION MET: guest JNI_OnLoad ran, natives registered ===\n");

    // load() takes the *directory* — it appends "/libunity.so" itself.
    phase("NativeLoader.load");
    // Before load, not before initJni: newer libunity asks ALooper_forThread()
    // for the UI thread inside its own JNI_OnLoad, which runs during this call.
    // Marking afterwards left "Couldn't retrieve native ALooper for UI thread."
    // in every affected Unity boot.
    kl_jni_mark_ui_thread();
    P(out, "\n=== NativeLoader.load(\"%s\") ===\n", g_libdir);
    kl_jni_local_frame_push();
    int8_t ok = ((nativeloader_load_fn)load)(kl_jni_env(), NULL,
                                             kl_jni_new_string(g_libdir));
    kl_jni_local_frame_pop();
    P(out, "  NativeLoader.load returned %d\n", ok);
    if (!ok) return fail("NativeLoader.load could not bring up libunity.so");

    // UnityPlayer's constructor calls initJni(Context) first. It is
    // `private final native`, so the guest sees (JNIEnv*, jobject thiz, jobject
    // context). The Context must be the Activity — the manifest declares
    // UnityPlayerActivity and Unity checks with IsInstanceOf — and the shared
    // singleton, because Unity reads it back through the static
    // UnityPlayer.currentActivity and compares.
    // Two initJni generations. Unity 2021's is (Context) alone; the 2022+/Meta
    // template (newer Meta Unity guests) registers
    // (Landroid/content/Context;ILjava/lang/String;)V - context, CONTEXT TYPE,
    // command line. Calling the new one with the old arity left the int and
    // String registers holding garbage: libunity then logged the junk forever
    // ("Unknown context type: 31 / <stale pointer>", 4069 times) and trapped
    // with SIGILL before the first frame. The int is com.unity3d.player.a.o's
    // enum value - ActivityOrService = 0, which UnityPlayerActivity uses - and
    // the string is the "unity" intent extra, empty on a plain launch.
    // Arity is decided from the signature RegisterNatives actually stored, not
    // from an exact-string lookup: the exact match came back NULL on a run whose
    // log showed the 3-arg registration, and printing the stored bytes verbatim
    // is the only way the next log can say why. strstr on the context-type int
    // plus the command-line String is the discriminator between generations.
    const char *ijsig = kl_jni_native_sig("com/unity3d/player/UnityPlayer", "initJni");
    void *initJni = kl_jni_native("com/unity3d/player/UnityPlayer", "initJni", NULL);
    if (!initJni) return fail("UnityPlayer.initJni was never registered");
    // THREE initJni generations, decided from the signature RegisterNatives
    // actually stored:
    //   1-arg  (Landroid/content/Context;)V                         Unity 2021
    //   2-arg  (Landroid/content/Context;I)V                        context + type
    //   3-arg  (Landroid/content/Context;ILjava/lang/String;)V      + command line
    // The int is the context-type enum (0 = ActivityOrService, what
    // UnityPlayerActivity uses); the String is the "unity" intent extra, empty
    // on a plain launch. Calling a newer arity with an older one leaves the int
    // (and String) registers holding garbage, and libunity logs "Unknown context
    // type: <junk>" forever and SIGILLs before the first frame — which is exactly
    // what Mission ISS's (Context;I) form did when it fell to the 1-arg branch.
    int initJni3 = ijsig && strstr(ijsig, "ILjava/lang/String;") != NULL;
    int initJni2 = !initJni3 && ijsig && strstr(ijsig, ";I)") != NULL;
    phase("initJni");
    P(out, "\n=== UnityPlayer.initJni%s -> %s-arg call ===\n",
      ijsig ? ijsig : "<sig unrecorded>", initJni3 ? "3" : initJni2 ? "2" : "1");
    // The shared singleton for the same reason the Context is one: this is the
    // object UnityPlayerActivity keeps in mUnityPlayer, and the guest reads it
    // back through that field. See kl_jni.h.
    void *thiz = kl_jni_unity_player();
    kl_jni_local_frame_push();
    if (initJni3)
        ((void (*)(void *, void *, void *, int, void *))initJni)(
            kl_jni_env(), thiz, kl_jni_activity(), 0, kl_jni_new_string(""));
    else if (initJni2)
        ((void (*)(void *, void *, void *, int))initJni)(
            kl_jni_env(), thiz, kl_jni_activity(), 0);
    else
        ((void (*)(void *, void *, void *))initJni)(kl_jni_env(), thiz, kl_jni_activity());
    kl_jni_local_frame_pop();
    P(out, "  initJni returned\n");
    // Here rather than at libmain: the chain dlopens libunity.so and libil2cpp.so
    // on its way through initJni, so this is the first moment the map is complete.
    kl_dl_report_images(out);
    // ...and the rest of that same constructor: the helper objects hand
    // THEMSELVES to libunity, and it does not null-check the handles.
    kl_jni_unity_construct_helpers();
    P(out, "\n=== EXIT CRITERION MET: initJni completed, no unimplemented "
           "JNI calls ===\n");
    return 0;
}

// Steam Link's seven natives are STATIC exports, resolved by name off libmain
// rather than through RegisterNatives — the mix is the measurement, and it is
// the streaming client's alone: the shell has no video surface to hand anyone.
static const char *const SL_STATIC_NATIVES[] = {
    "Java_com_valvesoftware_steamlink_SteamLink_useVideoSurface",
    "Java_com_valvesoftware_steamlink_SteamLink_videoSurfaceCreated",
    "Java_com_valvesoftware_steamlink_SteamLink_videoSurfaceDestroyed",
    "Java_com_valvesoftware_steamlink_SteamLink_overlaySurfaceCreated",
    "Java_com_valvesoftware_steamlink_SteamLink_overlaySurfaceDestroyed",
    "Java_com_valvesoftware_steamlink_SteamLink_freezeRendering",
    "Java_com_valvesoftware_steamlink_SteamLink_thawRendering",
};

static int boot_steamlink(FILE *out) {
    P(out, "=== Steam Link: %s front door (%s -> %s), %s ===\n",
      kl_slink_door_name(), kl_slink_main_lib(), kl_slink_main_fn(), g_libdir);

    // Mapping and relocation are separated from DT_INIT_ARRAY on purpose: an
    // unresolved import aborts by name when it is CALLED and the first init
    // array calls one, so running inits as we go would stop the run before the
    // gap could be printed and the shim work list would arrive one symbol per
    // rebuild.
    phase("steamlink chain");
    P(out, "=== mapping the working set ===\n");
    if (kl_slink_load_chain(out) != 0) return fail(kl_slink_error());
    kl_slink_report_gap(out);
    if (g_gap_only) return 0;

    phase("steamlink inits");
    P(out, "\n=== DT_INIT_ARRAY, dependencies first ===\n");
    kl_slink_run_inits(out);

    // The VR front door diverges here and never rejoins. Everything below is
    // SDL3's, and libvrlink_scene has none of it: no libSDL3 in its DT_NEEDED,
    // no JNI_OnLoad export, no natives to register. Its whole entry is the one
    // function Android's NativeActivity dlsyms.
    if (g_door == KL_SLINK_VR) {
        P(out, "\n=== EXIT CRITERION MET: the VR chain is bound and "
               "initialised ===\n");
        return 0;
    }

    phase("SDL3 JNI_OnLoad");
    P(out, "\n=== libSDL3.so JNI_OnLoad ===\n");
    if (kl_slink_sdl_onload(out) != 0) return fail(kl_slink_error());

    if (g_door == KL_SLINK_CLIENT) {
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", g_libdir, kl_slink_main_lib());
        kl_image *entry = kl_find_image(path);
        unsigned n = 0;
        for (size_t i = 0; entry && i < sizeof SL_STATIC_NATIVES / sizeof *SL_STATIC_NATIVES; i++)
            if (kl_sym(entry, SL_STATIC_NATIVES[i])) n++;
        P(out, "  libmain static Java_* natives resolved: %u/%zu\n",
          n, sizeof SL_STATIC_NATIVES / sizeof *SL_STATIC_NATIVES);
    }

    // SDL.setupJNI() caches the activity class and every method id SDL3 will
    // call back through — the densest single JNI call in the app, and therefore
    // where the surface starts failing by name.
    phase("SDL.setupJNI");
    P(out, "\n=== SDL.setupJNI() ===\n");
    kl_slink_sdl_setup(out);
    P(out, "\n=== EXIT CRITERION MET: the chain is bound, SDL3 JNI_OnLoad "
           "ran ===\n");
    return 0;
}

static int boot_ue4(FILE *out) {
    phase("ue4 configure");
    if (kl_ue4_configure(g_libdir, g_target->entry_lib, out) != 0) return fail(kl_ue4_error());
    phase("ue4 chain");
    if (kl_ue4_load(out) != 0) return fail(kl_ue4_error());
    P(out, "\n=== the shim gap ===\n");
    kl_ue4_gap(out);
    if (g_gap_only) return 0;
    P(out, "\n=== EXIT CRITERION MET: the Unreal chain is bound and "
           "initialised ===\n");
    return 0;
}

static int boot_jkxr(FILE *out) {
    phase("jkxr configure");
    // The libdir must be ABSOLUTE for this door: the engine chdirs into its own
    // data directory inside onCreate, and every relative path handed to it stops
    // resolving at that moment.
    if (kl_jkxr_configure(g_libdir, g_target->entry_lib, out) != 0)
        return fail(kl_jkxr_error());
    phase("jkxr chain");
    if (kl_jkxr_load(out) != 0) return fail(kl_jkxr_error());
    P(out, "\n=== the shim gap ===\n");
    kl_jkxr_gap(out);
    if (g_gap_only) return 0;
    P(out, "\n=== EXIT CRITERION MET: the OpenJK chain is bound and "
           "initialised ===\n");
    return 0;
}

static int boot_native(FILE *out) {
    phase("native configure");
    if (kl_native_configure(g_libdir, g_target->entry_lib, out) != 0)
        return fail(kl_native_error());
    phase("native load");
    if (kl_native_load(out) != 0) return fail(kl_native_error());
    P(out, "\n=== the shim gap ===\n");
    kl_native_gap(out);
    if (g_gap_only) return 0;
    P(out, "\n=== EXIT CRITERION MET: the NativeActivity library is mapped and "
           "initialised ===\n");
    return 0;
}

static int boot_sdl2(FILE *out) {
    phase("sdl2 configure");
    if (kl_sdl2_configure(g_libdir, g_target->entry_lib, out) != 0) return fail(kl_sdl2_error());
    phase("sdl2 load");
    if (kl_sdl2_load(out) != 0) return fail(kl_sdl2_error());
    P(out, "\n=== the shim gap ===\n");
    kl_sdl2_gap(out);
    if (g_gap_only) return 0;
    P(out, "\n=== EXIT CRITERION MET: the SDL2 graph is mapped and JNI_OnLoad ran ===\n");
    return 0;
}

static int boot_gles3jni(FILE *out) {
    phase("gles3jni configure");
    if (kl_gles3jni_configure(g_libdir, g_target->entry_lib, out) != 0) return fail(kl_gles3jni_error());
    phase("gles3jni load");
    if (kl_gles3jni_load(out) != 0) return fail(kl_gles3jni_error());
    P(out, "\n=== the shim gap ===\n");
    kl_gles3jni_gap(out);
    if (g_gap_only) return 0;
    P(out, "\n=== EXIT CRITERION MET: the GLES3JNILib engine is mapped and JNI_OnLoad ran ===\n");
    return 0;
}

// The Linux process door. "Boot" is where its world is put together — mounts,
// links, environment — and stops short of starting it, like every other door's.
// KL_LX_ROOT is the staged tree and KL_LX_DYLIBS where the translations are; the
// app sets both from its own bundle and container.
static int boot_linux(FILE *out) {
    phase("linux configure");
    if (kl_linuxproc_configure(kl_env_str("KL_LX_ROOT", NULL), kl_env_str("KL_LX_DYLIBS", NULL),
                               out) != 0)
        return fail(kl_linuxproc_error());
    if (g_gap_only) return 0;
    P(out, "\n=== EXIT CRITERION MET: the Linux process is described and its tree is staged ===\n");
    return 0;
}

int kl_driver_boot(FILE *out) {
    if (kl_extension_start() != 0) return fail(kl_extension_error());
    if (!g_target) return fail("kl_driver_init was not called");
    g_gap_only = kl_env_on("KL_GAP_ONLY", 0);
    int rc;
    switch (kl_driver_kind()) {
    case KL_GUEST_STEAMLINK: rc = boot_steamlink(out); break;
    case KL_GUEST_UE4:       rc = boot_ue4(out);       break;
    case KL_GUEST_JKXR:      rc = boot_jkxr(out);      break;
    case KL_GUEST_NATIVE:    rc = boot_native(out);    break;
    case KL_GUEST_SDL2:      rc = boot_sdl2(out);      break;
    case KL_GUEST_LINUX:     rc = boot_linux(out);     break;
    case KL_GUEST_GLES3JNI:  rc = boot_gles3jni(out);  break;
    default:                 rc = boot_unity(out);     break;
    }
    if (rc == 0 && g_gap_only)
        P(out, "\n(KL_GAP_ONLY: stopping at the shim gap — nothing was run)\n");
    return rc;
}

// ---- the lifecycle ---------------------------------------------------------

// Unity 2022+/Meta template split the player: initJni stays on UnityPlayer, but
// the lifecycle natives (nativeRecreateGfxState/Resume/Render) moved onto the
// UnityPlayerForActivityOrService subclass. Some guests registered them only
// there, so a UnityPlayer-only lookup came back "not registered" and the frame
// loop ended before the first frame. Prefer the subclass, fall back to the base
// so 2019/2021 titles (which register on UnityPlayer) are unchanged. Identity of
// thiz does not matter here: initJni and these calls already use different
// synthetic objects, so libunity keys its state off native globals, not thiz.
static void *unity_lifecycle_native(const char *name) {
    void *fn = kl_jni_native("com/unity3d/player/UnityPlayerForActivityOrService",
                             name, NULL);
    return fn ? fn : kl_jni_native("com/unity3d/player/UnityPlayer", name, NULL);
}

// KL_SAMPLE_MS: the guest-thread sampler, wired here so it runs on DEVICE
// too - m_boot starts it around its own pump, but the visionOS app pumps
// through this driver and never saw the knob. Metadata for managed-name
// resolution comes from KL_IL2CPP_METADATA or the libdir-derived path; on
// device the tree usually is not beside the libs, and the sampler then
// reports module+offset frames, which is the useful part on v29 metadata.
//
// A helper because every DOOR needs it, not just Unity's: the first wrath2
// freeze-hunt ran with KL_SAMPLE_MS set and got nothing back — the knob was
// honoured only in begin_unity, and a UE4 guest booted through begin_ue4,
// which never looked. (A UE4 guest has no il2cpp metadata; the sampler then
// reports module+offset frames, which is the half that matters there anyway.)
static void sample_start_if_asked(void) {
    const char *senv = getenv("KL_SAMPLE_MS");
    if (!senv || !*senv) return;
    char meta[1024];
    const char *mp = getenv("KL_IL2CPP_METADATA");
    if (!mp) {
        snprintf(meta, sizeof meta, "%s", g_libdir);
        char *tail = strstr(meta, "/lib/");
        if (tail) *tail = 0;
        snprintf(meta + strlen(meta), sizeof meta - strlen(meta),
                 "/assets/bin/Data/Managed/Metadata/global-metadata.dat");
        mp = meta;
    }
    kl_sample_start((unsigned)strtoul(senv, NULL, 10), mp);
}

static int begin_unity(FILE *out) {
    // The SAME player initJni ran on, not a second instance — see kl_jni.h.
    g_thiz = kl_jni_unity_player();
    if (!g_thiz) return fail("kl_driver_boot must run first");

    // Index the OBB's audio banks by basename (NULL base = no UE4 loose-path
    // serving, banks only). A Unity sound engine — Wwise (ZIX) or FMOD — reads
    // its .bnk/.wem/.bank by a native path unrelated to the OBB layout, and the
    // banks live only inside the OBB; without this the engine gets AK_FileNotFound
    // and renders silence.
    kl_obbmap_init(kl_jni_obb_dir(), NULL);

    // The order UnityPlayerActivity drives: attach a surface, resume, then one
    // frame. nativeRecreateGfxState is what reaches for EGL, and it is also what
    // pulls in libil2cpp — the image the boot gate never loads.
    void *surface = kl_jni_new_object("android/view/Surface");
    struct { const char *name; int kind; } seq[] = {
        { "nativeRecreateGfxState", 2 }, { "nativeResume", 0 }, { "nativeRender", 1 },
    };
    for (unsigned i = 0; i < sizeof seq / sizeof seq[0]; i++) {
        void *fn = unity_lifecycle_native(seq[i].name);
        if (!fn) { P(out, "  %s: not registered\n", seq[i].name); continue; }
        phase(seq[i].name);
        P(out, "\n=== UnityPlayer.%s ===\n", seq[i].name);
        // The render loop may block; the watchdog is what names WHERE, since a
        // SIGALRM is reported as "still alive and blocked" rather than a crash.
        if (g_alarm) alarm(g_alarm);
        kl_jni_local_frame_push();
        if (seq[i].kind == 2)
            ((void (*)(void *, void *, int, void *))fn)(kl_jni_env(), g_thiz, 0, surface);
        else if (seq[i].kind == 1)
            P(out, "  -> %d\n", ((int8_t (*)(void *, void *))fn)(kl_jni_env(), g_thiz));
        else
            ((void (*)(void *, void *))fn)(kl_jni_env(), g_thiz);
        kl_jni_local_frame_pop();
        if (g_alarm) alarm(0);
        P(out, "  %s returned\n", seq[i].name);
        // Android's UI thread runs its looper between callbacks; here nothing
        // else will, so the queue would only grow.
        if (g_alarm) alarm(g_alarm);
        unsigned ran = kl_jni_drain_ui_tasks();
        if (g_alarm) alarm(0);
        if (ran) P(out, "  drained %u posted task%s\n", ran, ran == 1 ? "" : "s");
    }

    g_render = unity_lifecycle_native("nativeRender");
    // The graphics device exists by now and no frame has been drawn yet. The
    // raise lets libunity bind past its un-queried cap of 32 texture units
    // instead of refusing every bind and reading stale unit-0 textures; it is
    // declined per Unity build, and names the build when it declines.
    kl_guest_poke_texture_unit_cap();
    sample_start_if_asked();
    phase("frame pump");
    return 0;
}

static int begin_steamlink(FILE *out) {
    if (g_door != KL_SLINK_VR) {
        phase("SDL onCreate -> main");
        P(out, "\n=== onCreate -> %s ===\n", kl_slink_main_fn());
        if (kl_slink_sdl_start_main(out) != 0) return fail(kl_slink_error());
        phase("guest running");
        return 0;
    }
    phase("ANativeActivity_onCreate");
    P(out, "\n=== ANativeActivity_onCreate (the VR front door) ===\n");
    if (kl_slink_vr_create(out) != 0) return fail(kl_slink_error());
    phase("activity lifecycle");
    P(out, "\n=== the activity lifecycle ===\n");
    kl_slink_vr_start(out);
    phase("looper pump");
    return 0;
}

static int begin_ue4(FILE *out) {
    phase("ANativeActivity_onCreate");
    P(out, "\n=== ANativeActivity_onCreate ===\n");
    if (g_alarm) alarm(g_alarm);
    if (kl_ue4_create(out) != 0) { if (g_alarm) alarm(0); return fail(kl_ue4_error()); }
    kl_ue4_start(out);
    if (g_alarm) alarm(0);
    sample_start_if_asked();
    phase("looper pump");
    return 0;
}

static int begin_jkxr(FILE *out) {
    phase("GLES3JNILib.onCreate");
    P(out, "\n=== GLES3JNILib.onCreate ===\n");
    if (g_alarm) alarm(g_alarm);
    if (kl_jkxr_create(out) != 0) { if (g_alarm) alarm(0); return fail(kl_jkxr_error()); }
    // Where the engine will look for its data, which nothing else can say. id
    // Tech 3 takes fs_basepath from the cwd the guest chdir'd to inside onCreate
    // and fs_homepath from $HOME; a chdir that failed leaves the process where it
    // started and every pk3 is then looked for in `<cwd>/base`. This port drops
    // its own console output, so the engine's report of that reaches the log as
    // an exit and nothing else.
    char cwd[1200];
    P(out, "  [jkxr] cwd after onCreate (the engine's fs_basepath): %s\n",
      getcwd(cwd, sizeof cwd) ? cwd : strerror(errno));
    P(out, "  [jkxr] HOME (the engine's fs_homepath root): %s\n",
      getenv("HOME") ? getenv("HOME") : "(unset)");
    // onStart / onResume / surfaceCreated / surfaceChanged. The surface is where
    // the engine stops waiting — its render thread blocks until one arrives — so
    // a begin that stopped at onCreate would look like a hang.
    kl_jkxr_start(out);
    if (g_alarm) alarm(0);
    sample_start_if_asked();
    phase("looper pump");
    return 0;
}

static int begin_sdl2(FILE *out) {
    phase("SDLActivity begin");
    P(out, "\n=== SDLActivity: setupJNI + nativeRunMain ===\n");
    if (g_alarm) alarm(g_alarm);
    int rc = kl_sdl2_begin(out);
    if (g_alarm) alarm(0);
    if (rc != 0) return fail(kl_sdl2_error());
    sample_start_if_asked();
    phase("looper pump");
    return 0;
}

static int begin_gles3jni(FILE *out) {
    phase("GLES3JNILib onCreate");
    P(out, "\n=== GLES3JNILib: onCreate + lifecycle ===\n");
    if (g_alarm) alarm(g_alarm);
    if (kl_gles3jni_create(out) != 0) { if (g_alarm) alarm(0); return fail(kl_gles3jni_error()); }
    if (g_alarm) alarm(0);
    // onStart/onResume/onSurfaceCreated/Changed — the surface is where the engine
    // stops waiting, so a begin that stopped at onCreate would look like a hang.
    kl_gles3jni_start(out);
    sample_start_if_asked();
    phase("looper pump");
    return 0;
}

static int begin_native(FILE *out) {
    phase("ANativeActivity_onCreate");
    P(out, "\n=== ANativeActivity_onCreate ===\n");
    if (g_alarm) alarm(g_alarm);
    if (kl_native_create(out) != 0) { if (g_alarm) alarm(0); return fail(kl_native_error()); }
    if (g_alarm) alarm(0);
    // onStart / onResume / onNativeWindowCreated: the game's render thread has
    // been spinning since onCreate and blocks until the window arrives, so a begin
    // that stopped at onCreate would look like a hang.
    kl_native_start(out);
    sample_start_if_asked();
    phase("looper pump");
    return 0;
}

static int begin_linux(FILE *out) {
    phase("linux exec");
    P(out, "\n=== starting the Linux process ===\n");
    if (kl_linuxproc_begin(out) != 0) return fail(kl_linuxproc_error());
    sample_start_if_asked();
    phase("linux process");
    return 0;
}

int kl_driver_lifecycle_begin(FILE *out) {
    if (!g_target) return fail("kl_driver_init was not called");
    switch (kl_driver_kind()) {
    case KL_GUEST_STEAMLINK: return begin_steamlink(out);
    case KL_GUEST_UE4:       return begin_ue4(out);
    case KL_GUEST_JKXR:      return begin_jkxr(out);
    case KL_GUEST_NATIVE:    return begin_native(out);
    case KL_GUEST_SDL2:      return begin_sdl2(out);
    case KL_GUEST_LINUX:     return begin_linux(out);
    case KL_GUEST_GLES3JNI:  return begin_gles3jni(out);
    default:                 return begin_unity(out);
    }
}

// KL_UNITY_QUIT_TEARDOWN: when nativeRender first answers false (the game's
// own quit), give Unity the end Android would: the game's preferences saved,
// then nativePause, then nativeDone, on the thread that just ran nativeRender.
// nativeRender is dropped after it, so the engine that was torn down is never
// called again whatever the frame loop does next.
//
// Gun Club VR (x2285803881494677, build 410) is the one title it was run with,
// on its sender's headset (fix 44, 2026-10-15; not run here): its in-game Exit
// left its settings at their old values, since pause and teardown alone wrote
// nothing, and with TBMPrefs.Save in front of them they were kept across a
// restart. Default on for that title only, alongside KL_UNITY_QUIT_EXITS.
static int unity_quit_teardown(void) {
    static int v = -1;
    if (v < 0) {
        const char *t = kl_driver_target_name();
        v = kl_env_on("KL_UNITY_QUIT_TEARDOWN", t && strcmp(t, "x2285803881494677") == 0);
    }
    return v;
}

static void unity_lifecycle_call(const char *name) {
    void *fn = unity_lifecycle_native(name);
    if (!fn) { fprintf(stderr, "[guest] quit: %s not registered\n", name); return; }
    kl_jni_local_frame_push();
    ((void (*)(void *, void *))fn)(kl_jni_env(), g_thiz);
    kl_jni_local_frame_pop();
    fprintf(stderr, "[guest] quit: %s returned\n", name);
}

static void unity_quit_teardown_run(void) {
    fprintf(stderr, "[guest] nativeRender returned false: saving and tearing down "
                    "(KL_UNITY_QUIT_TEARDOWN)\n");
    kl_mprobe_gunclub_save_preferences();
    unity_lifecycle_call("nativePause");
    unity_lifecycle_call("nativeDone");
    g_render = NULL;
}

int kl_driver_frame(void) {
    if (kl_driver_owns_frame_loop()) return -1;
    if (!g_render || !g_thiz) return -1;
    if (g_alarm) alarm(g_alarm);
    // Pin this frame's poses before anything in the frame can ask, so every
    // ovrp_GetNodePoseState inside it answers the same thing and the pose
    // recorded for timewarp is the one the picture was drawn from. A guest frame
    // is longer than a display frame whenever performance is short, and without
    // this the head moves INSIDE the frame.
    kl_ovrp_frame_latch();
    // No Choreographer tick here: doFrame comes from a free-running host thread
    // started at the guest's first postFrameCallback (kl_jni_looper.c), because
    // the engine waits inside nativeRender on a refresh counter each doFrame
    // advances. A tick from this loop is a second source handing the engine two
    // doFrames per rendered frame, which is its frame delta halved.
    kl_jni_local_frame_push();
    int r = ((int8_t (*)(void *, void *))g_render)(kl_jni_env(), g_thiz);
    kl_jni_local_frame_pop();
    kl_jni_drain_ui_tasks();
    kl_jni_audioinaec_pump();
    if (r == 0 && unity_quit_teardown()) {
        // Nothing else runs against the engine this frame: no probes, no songs.
        unity_quit_teardown_run();
        if (g_alarm) alarm(0);
        g_frames++;
        return r;
    }
    // Android's low-memory notification: one task_info call, and the guest drops
    // its caches when the footprint crosses the reported budget.
    kl_mem_pressure_poll();
    // KL_PROBE_INPUT: the managed-side probe, self-gated on its env knob and
    // wired here so it runs on DEVICE too (m_boot ticks it around its own
    // pump). Between frames on the thread that just ran one, which is where
    // managed calls are safe.
    kl_mprobe_tick(g_frames);
    // AC Nexus's own half-rate spin, taken out (self-gated on the target).
    kl_mprobe_nexus_frame_budget(g_frames);
    // Custom levels, from the same seat and for the same reason. Self-gated on a
    // CustomLevels directory existing, so every other guest pays one stat().
    kl_songs_tick(g_frames);
    if (g_alarm) alarm(0);
    g_frames++;
    return r;
}

double kl_driver_pump(double seconds, const volatile int *quit) {
    switch (kl_driver_kind()) {
    case KL_GUEST_STEAMLINK:
        return g_door == KL_SLINK_VR ? kl_slink_vr_pump(seconds, quit)
                                     : kl_slink_sdl_pump(seconds, quit);
    case KL_GUEST_UE4:  return kl_ue4_pump(seconds, quit);
    case KL_GUEST_JKXR: return kl_jkxr_pump(seconds, quit);
    case KL_GUEST_NATIVE: return kl_native_pump(seconds, quit);
    case KL_GUEST_SDL2: return kl_sdl2_pump(seconds, quit);
    case KL_GUEST_LINUX: return kl_linuxproc_pump(seconds, quit);
    case KL_GUEST_GLES3JNI: return kl_gles3jni_pump(seconds, quit);
    default:            return 0.0;
    }
}

double kl_driver_pump_default(void) {
    switch (kl_driver_kind()) {
    // 10 s measures how far the guest gets by itself; the pairing loop wants
    // longer and says so with the knob.
    case KL_GUEST_STEAMLINK: return (double)kl_env_uint("KL_SLINK_WAIT", 10);
    // RE4 needs 300: the first minute is the engine's one-time shader
    // optimization.
    case KL_GUEST_UE4:       return kl_env_str("KL_UE4_WAIT", NULL)
                                  ? strtod(getenv("KL_UE4_WAIT"), NULL) : 5.0;
    case KL_GUEST_JKXR:      return kl_env_str("KL_JKXR_WAIT", NULL)
                                  ? strtod(getenv("KL_JKXR_WAIT"), NULL) : 5.0;
    case KL_GUEST_NATIVE:
    case KL_GUEST_SDL2:
    case KL_GUEST_GLES3JNI:  return kl_env_str("KL_NATIVE_WAIT", NULL)
                                  ? strtod(getenv("KL_NATIVE_WAIT"), NULL) : 5.0;
    // A minute: long enough for a small program to finish and for an engine to
    // say how far it gets without a compositor.
    case KL_GUEST_LINUX:     return kl_env_str("KL_LX_WAIT", NULL)
                                  ? strtod(getenv("KL_LX_WAIT"), NULL) : 60.0;
    default:                 return 0.0;
    }
}

void kl_driver_report(FILE *out) {
    if (!out) return;
    switch (kl_driver_kind()) {
    case KL_GUEST_STEAMLINK:
        fprintf(out, "\n=== the Steam Link %s run ===\n", kl_slink_door_name());
        kl_slink_report(out);
        return;
    case KL_GUEST_UE4:
        fprintf(out, "\n=== the Unreal run ===\n");
        kl_ue4_report(out);
        fflush(out);
        return;
    case KL_GUEST_JKXR:
        fprintf(out, "\n=== the OpenJK run ===\n");
        kl_jkxr_report(out);
        fflush(out);
        return;
    case KL_GUEST_NATIVE:
        fprintf(out, "\n=== the NativeActivity run ===\n");
        kl_native_report(out);
        fflush(out);
        return;
    case KL_GUEST_SDL2:
        fprintf(out, "\n=== the SDL2 run ===\n");
        kl_sdl2_report(out);
        return;
    case KL_GUEST_LINUX:
        fprintf(out, "\n=== the Linux process run ===\n");
        kl_linuxproc_report(out);
        return;
    case KL_GUEST_GLES3JNI:
        fprintf(out, "\n=== the GLES3JNILib run ===\n");
        kl_gles3jni_report(out);
        return;
    default: break;
    }
    fprintf(out, "  pumped %u frames\n", g_frames);
    kl_jni_report(out);
    kl_egl_report(out);
    kl_opensl_report(out);
    // A Unity guest can reach the video path too — Open Brush seeds a video into
    // its own media library at startup.
    kl_mediandk_report(out);
    kl_ovrp_report(out);
    kl_ovrplat_report(out);
    kl_steamapi_report(out);
    kl_openxr_report(out);
    kl_madv_report();
    kl_mem_report();
    fflush(out);
}
