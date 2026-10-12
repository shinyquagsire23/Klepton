// org.fmod — FMOD's Android Java helper classes.
//
// FMOD's native core (libfmod.so) does not open the audio device itself on
// Android through raw syscalls; it goes through a small Java shim, org.fmod.FMOD
// (from fmod.jar), which owns the Android Context and answers the platform
// queries the native mixer needs — output sample rate, block size, whether the
// low-latency / AAudio fast paths are available. FMOD's JNI_OnLoad resolves this
// class and org.fmod.{AudioDevice,MediaCodec}; the native side then calls
// FMOD.checkInit() before it will bring the core up. With no host implementation
// the checkInit() call aborted (vampire), so FMOD::getGlobals / System::create
// never ran and there was no audio.
//
// There is no real Java here, so this answers the class synthetically, and it
// answers the WHOLE public surface rather than the handful of names one title
// happened to reach first. Unreal titles reach these from libfmod's own init;
// Unity titles also reach them from C# (HARD BULLET, Paint the Town Red VR,
// GORN 2), and the FMOD releases they ship differ: the newer ones add
// registerHeadsetDetection, getDevices and friends, and every missing name is a
// fatal "no host implementation" at the moment it is first called.
//
// Every value here must AGREE with the answer this runtime gives to the same
// question asked another way, not merely be plausible. FMOD sizes its mixer
// block from getOutputBlockSize and its rate from getOutputSampleRate, and a
// mixer configured for a device the open call then refuses is worse than no
// sound:
//
//   getOutputSampleRate  KLJ_AUDIO_RATE     — the same two numbers
//   getOutputBlockSize   KLJ_AUDIO_FRAMES     AudioManager.getProperty gives,
//                                             which kl_aaudio.c then opens with
//   lowLatencyFlag       true               — PackageManager.hasSystemFeature
//   proAudioFlag         false                answers exactly this pair
//   isBluetoothOn        false              — AudioManager.isBluetoothA2dpOn
//   getDevices           [1]                — AudioManager.getDevices (one
//                                             output, the built-in speaker)
//   getAssetManager      the one AssetManager Context.getAssets hands out
//
// supportsAAudio stays FALSE by default, which keeps FMOD on the OpenSL ES
// output (kl_opensl.c) this file has always selected. In fmod.jar it is
// `const/4 v0, 0x1; return v0` — a compile-time constant — so on a Quest FMOD
// takes AAudio; `KL_FMOD_AAUDIO=1` answers true and does the same here, through
// kl_aaudio.c. Unity FMOD titles (HARD BULLET, Paint the Town Red VR, GORN 2)
// have run on the AAudio answer.
//
// supportsSpatial is false. Android's Spatializer is not modelled, and FMOD
// titles here ship their own spatialiser (OculusSpatializerFMOD,
// resonanceaudio) — the path a false answer selects.
//
// supportsLowLatency is implemented as fmod.jar's own computation over the
// values above rather than as an independent opinion: block size in (0,1024],
// low latency true, Bluetooth false -> true. If any of those changes this one
// has to follow, which is why it is written as the derivation.
#include <stdint.h>
#include <stdlib.h>
#include "kl_jni.h"
#include "kl_jni_int.h"
#include "kl_env.h"

#define FMODCLS "org/fmod/FMOD"

static klj_val klf_true(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n; return (klj_val){.j = 1};
}
static klj_val klf_false(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n; return (klj_val){.j = 0};
}

// The Android Context FMOD stashes and hands back to its own code. The activity
// is the context here, exactly as GameActivity is elsewhere.
static klj_val klf_getContext(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = kl_jni_activity()};
}

// init(Context) / close(). The Java body stores the Context and registers a
// HEADSET_PLUG receiver and an AudioDeviceCallback. Neither callback can ever
// fire here — the host's device set does not change under us — and the Context
// is always the activity (getContext above), so both reduce to a log line.
static klj_val klf_init(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("FMOD.init(Context) — the context is the activity; no headset/device "
            "callbacks exist to register on this host");
    return (klj_val){0};
}

// checkInit() is `gContext != null`. On Android that is only true after
// init(Context); here the Context exists before the guest does, and an Unreal
// guest never calls init at all (GameActivity.java would, and it does not run
// here) — so the gate is always open. Answering from an init() flag would leave
// every Unreal FMOD title with no core system and no sound.
static klj_val klf_checkInit(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.j = 1};
}

static klj_val klf_getAssetManager(void *env, void *self, const klj_val *a, int n) {
    return klj_Context_getAssets(env, self, a, n);
}

static klj_val klf_sampleRate(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("FMOD.getOutputSampleRate() -> %d", KLJ_AUDIO_RATE);
    return (klj_val){.j = KLJ_AUDIO_RATE};
}
static klj_val klf_blockSize(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("FMOD.getOutputBlockSize() -> %d", KLJ_AUDIO_FRAMES);
    return (klj_val){.j = KLJ_AUDIO_FRAMES};
}

static klj_val klf_supportsAAudio(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    int on = kl_env_on("KL_FMOD_AAUDIO", 0);
    KLJ_LOG("FMOD.supportsAAudio() -> %s", on ? "true (KL_FMOD_AAUDIO=1: kl_aaudio)"
                                              : "false (OpenSL ES)");
    return (klj_val){.j = (uint64_t)on};
}

static klj_val klf_supportsLowLatency(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    int block_ok = KLJ_AUDIO_FRAMES > 0 && KLJ_AUDIO_FRAMES <= 1024;
    int ok = block_ok && /* lowLatencyFlag */ 1 && !/* isBluetoothOn */ 0;
    KLJ_LOG("FMOD.supportsLowLatency() -> %s (block %d, low latency yes, "
            "bluetooth no)", ok ? "true" : "false", KLJ_AUDIO_FRAMES);
    return (klj_val){.j = (uint64_t)ok};
}

static klj_val klf_getAudioDevices(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    // The older FMOD's AudioDeviceInfo[] census. Empty: its only consumer is a
    // device-change watcher, and nothing changes here.
    return (klj_val){.l = klj_new_array('L', "android/media/AudioDeviceInfo", 0)};
}

// FMOD.getDevices(int flags) -> int[] (FMOD 2.02+): AudioManager.getDevices(flags)
// mapped through AudioDeviceInfo.getId(), or an empty array. kl_jni_net.c
// answers exactly one OUTPUT device (id 1, the built-in speaker) and no inputs,
// so this is the same answer through FMOD's door.
static klj_val klf_getDevices(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    int flags = n > 0 ? (int)(int32_t)a[0].j : 2;
    int outs = (flags & 2) != 0;
    void *arr = klj_new_array('I', NULL, outs ? 1 : 0);
    if (outs) ((int32_t *)klj_arr(arr)->data)[0] = 1;   /* KLJ_AUDIO_DEV_ID, kl_jni_net.c */
    KLJ_LOG("FMOD.getDevices(0x%x) -> %s", flags,
            outs ? "[1] (the built-in speaker output)" : "[] (no inputs)");
    return (klj_val){.l = arr};
}
// getDeviceName(id) / getDeviceType(id): the same walk over getDevices(ALL)
// keyed by getId(). One device exists: id 1, product "Oculus Quest",
// TYPE_BUILTIN_SPEAKER (2) — kl_jni_net.c's AudioDeviceInfo. Any other id is
// the Java's miss arm: "" / 0.
static klj_val klf_getDeviceName(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    int id = n > 0 ? (int)(int32_t)a[0].j : -1;
    return (klj_val){.l = kl_jni_new_string(id == 1 ? "Oculus Quest" : "")};
}
static klj_val klf_getDeviceType(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    int id = n > 0 ? (int)(int32_t)a[0].j : -1;
    return (klj_val){.j = id == 1 ? 2 : 0};
}
static klj_val klf_fileDescriptorFromUri(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    KLJ_LOG("FMOD.fileDescriptorFromUri(\"%s\") -> -1 (no ContentResolver here)",
            n > 0 ? klj_str(a[0].l) : "");
    return (klj_val){.j = (uint64_t)-1};   // the FileNotFoundException arm
}

// registerHeadsetDetection / deregisterHeadsetDetection — the pair the Java
// wraps around a single BroadcastReceiver:
//
//   register:   new IntentFilter("android.intent.action.HEADSET_PLUG");
//               gContext.registerReceiver(gPluginBroadcastReceiver, filter)
//   deregister: gContext.unregisterReceiver(gPluginBroadcastReceiver)
//
// and the receiver's whole body is `if (HEADSET_PLUG.equals(intent.getAction()))
// OutputAAudioHeadphonesChanged()` — a native in libfmod that tells the AAudio
// output its endpoint moved. On this host it can never fire: there is one audio
// endpoint, kl_aaudio.c holds it for the life of the process, and nothing
// broadcasts HEADSET_PLUG. Older FMOD releases registered the same receiver
// inside init(); newer ones split it into a call FMOD's core makes right after
// supportsAAudio(), which is why the name is new and the behaviour is not.
//
// Both spellings of the remover are bound: Paint the Town Red VR's FMOD
// declares `deregisterHeadsetDetection`, other releases spell it
// `unregisterHeadsetDetection`.
static klj_val klf_headsetDetection(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("FMOD headset detection — recorded; this host has one audio "
            "endpoint and never broadcasts HEADSET_PLUG, so the receiver "
            "could not fire");
    return (klj_val){0};
}

// ---- org.fmod's two output classes and its decoder ----
//
// FMOD picks its Android output at init: AAudio or OpenSL ES. The
// AudioTrack path through org.fmod.AudioDevice / FMODAudioDevice is the
// FALLBACK and is expected to stay unused. It is bound anyway, and bound
// loudly: if FMOD ever does fall back, the log names the moment instead of the
// process aborting on a class libfmod FindClass'd at JNI_OnLoad.
//
// init() answers false — "this device could not be opened" — which is a real
// AudioTrack failure code FMOD handles by reporting an output error, rather than
// a yes that would have FMOD write into a device that is not there and hear
// nothing.
static klj_val klf_AudioDevice_init(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self;
    KLJ_LOG("org/fmod/AudioDevice.init(%d, %d, %d, %d) -> false — FMOD is on AAudio "
            "or OpenSL here; a call to this is the AudioTrack FALLBACK, not a "
            "normal boot",
            n > 0 ? (int)a[0].j : 0, n > 1 ? (int)a[1].j : 0,
            n > 2 ? (int)a[2].j : 0, n > 3 ? (int)a[3].j : 0);
    return (klj_val){.j = 0};
}
static klj_val klf_AudioDevice_void(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("org/fmod/AudioDevice: lifecycle call on the unused AudioTrack path — recorded");
    return (klj_val){.j = 0};
}

// org.fmod.MediaCodec is FMOD's bridge to android.media.MediaCodec: a hardware
// decoder for a compressed stream FMOD hands off rather than decodes itself.
// There is no such decoder to bridge to here, and the caller wants PCM back, so
// init() DECLINES: false is MediaCodec's own "no codec could be configured for
// this stream", and FMOD's caller handles it by failing that one sound rather
// than the mixer. The accessors are what a declined codec reports, so a caller
// that ignores the false still reads a consistent empty stream.
static klj_val klf_MediaCodec_init(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    KLJ_LOG("org/fmod/MediaCodec.init() -> false (DECLINED: no platform decoder here). "
            "FMOD will fail this one sound");
    return (klj_val){.j = 0};
}
static klj_val klf_MediaCodec_read(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    // -1 is end-of-stream, not an error: a declined codec has no frames, and a
    // caller looping on 0 would spin forever.
    return (klj_val){.j = (uint64_t)(int64_t)-1};
}

const klj_binding klj_bind_fmod[] = {
    // The gate the native side checks before creating the core system.
    {FMODCLS, "checkInit",           "()Z",                    klf_checkInit},
    // Declared as fmod.jar declares it. A Unity guest asks with the runtime
    // type it holds (UnityPlayerGameActivity); the assignability tier in
    // kl_jni.c bridges the two.
    {FMODCLS, "init",                "(Landroid/content/Context;)V", klf_init},
    {FMODCLS, "init",                "(Ljava/lang/Object;)V",  klf_init},
    {FMODCLS, "close",               "()V",                    klf_false},
    {FMODCLS, "getContext",          "()Landroid/content/Context;", klf_getContext},
    {FMODCLS, "getContext",          "()Ljava/lang/Object;",   klf_getContext},
    {FMODCLS, "getAssetManager",     "()Landroid/content/res/AssetManager;", klf_getAssetManager},
    // Output format + fast-path capabilities the native mixer queries.
    {FMODCLS, "getOutputSampleRate", "()I",                    klf_sampleRate},
    {FMODCLS, "getOutputBlockSize",  "()I",                    klf_blockSize},
    {FMODCLS, "lowLatencyFlag",      "()Z",                    klf_true},
    {FMODCLS, "proAudioFlag",        "()Z",                    klf_false},
    {FMODCLS, "supportsAAudio",      "()Z",                    klf_supportsAAudio},
    {FMODCLS, "supportsSpatial",     "()Z",                    klf_false},
    {FMODCLS, "supportsLowLatency",  "()Z",                    klf_supportsLowLatency},
    {FMODCLS, "isBluetoothOn",       "()Z",                    klf_false},
    // The device census, both generations.
    {FMODCLS, "getDevices",          "(I)[I",                  klf_getDevices},
    {FMODCLS, "getDeviceName",       "(I)Ljava/lang/String;",  klf_getDeviceName},
    {FMODCLS, "getDeviceType",       "(I)I",                   klf_getDeviceType},
    {FMODCLS, "getAudioDevices",     "(I)[Landroid/media/AudioDeviceInfo;", klf_getAudioDevices},
    {FMODCLS, "fileDescriptorFromUri", "(Ljava/lang/String;)I", klf_fileDescriptorFromUri},
    {FMODCLS, "registerHeadsetDetection",   "()V",             klf_headsetDetection},
    {FMODCLS, "deregisterHeadsetDetection", "()V",             klf_headsetDetection},
    {FMODCLS, "unregisterHeadsetDetection", "()V",             klf_headsetDetection},

    // The AudioTrack fallback and the platform decoder. Both classes are
    // FindClass'd by libfmod's JNI_OnLoad on every boot; neither is expected to
    // be called.
    {"org/fmod/AudioDevice", "init",  "(IIII)Z", klf_AudioDevice_init},
    {"org/fmod/AudioDevice", "close", "()V",     klf_AudioDevice_void},
    {"org/fmod/AudioDevice", "write", "([BI)V",  klf_AudioDevice_void},
    {"org/fmod/FMODAudioDevice", "start",     "()V", klf_AudioDevice_void},
    {"org/fmod/FMODAudioDevice", "stop",      "()V", klf_AudioDevice_void},
    {"org/fmod/FMODAudioDevice", "close",     "()V", klf_AudioDevice_void},
    {"org/fmod/FMODAudioDevice", "run",       "()V", klf_AudioDevice_void},
    {"org/fmod/FMODAudioDevice", "isRunning", "()Z", klf_false},
    {"org/fmod/FMODAudioDevice", "startAudioRecord", "(III)I", klf_false},
    {"org/fmod/FMODAudioDevice", "stopAudioRecord",  "()V",    klf_AudioDevice_void},
    {"org/fmod/MediaCodec", "init",            "(J)Z",   klf_MediaCodec_init},
    {"org/fmod/MediaCodec", "read",            "([BI)I", klf_MediaCodec_read},
    {"org/fmod/MediaCodec", "seek",            "(I)V",   klf_false},
    {"org/fmod/MediaCodec", "release",         "()V",    klf_false},
    {"org/fmod/MediaCodec", "getLength",       "()J",    klf_false},
    {"org/fmod/MediaCodec", "getChannelCount", "()I",    klf_false},
    {"org/fmod/MediaCodec", "getSampleRate",   "()I",    klf_false},
    {0}
};
