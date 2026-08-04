// Synthetic JavaVM / JNIEnv (M3 bootstrap, M4 surface measurement).
//
// There is no JVM here and there will never be one. Beat Saber is a
// com.unity3d.player.UnityPlayerActivity app, not a NativeActivity: libmain.so
// exports JNI_OnLoad and nothing else, so *every* entry into guest code starts
// with a JNI call. We answer those calls ourselves.
//
// The tables are full — all 233 JNIEnv slots and all 8 JavaVM slots are
// populated, and every slot we have not implemented is a *named* abort rather
// than a NULL. That is the point: it turns "the guest wandered off" into
// "the guest called GetMethodID(android/os/Build, getFingerprint)". The JNI
// surface is measured this way, not guessed (PLANNING §6 M4).
#ifndef KL_JNI_H
#define KL_JNI_H
#include <stdint.h>
#include <stdio.h>

#define KL_JNI_VERSION_1_6 0x00010006

// Guest-visible types. Layout only — we never interpret a jobject.
typedef int32_t kl_jint;
typedef void   *kl_jobject;

// JNINativeMethod, as passed to RegisterNatives. 24 bytes, three pointers.
typedef struct {
    const char *name;
    const char *signature;
    void       *fnPtr;
} kl_jni_method;

// Both handles are "pointer to a pointer to a function table" — the guest does
// `ldr x8,[x0]; ldr x8,[x8,#slot*8]; blr x8` and nothing else.
void *kl_jni_vm(void);        // JavaVM*  — hand this to JNI_OnLoad
void *kl_jni_env(void);       // JNIEnv*  — for the calling thread

// Unknown lookups (FindClass of an unseen class, any GetMethodID/GetFieldID)
// abort by default so a bring-up run stops exactly where the surface ends.
// Permissive mode logs and returns a synthetic id instead, which collects the
// whole surface in one pass rather than one abort-fix-rerun cycle per symbol.
void kl_jni_set_permissive(int on);

// Natives the guest registered, by (class, name, signature). NULL if absent.
void *kl_jni_native(const char *cls, const char *name, const char *sig);

// Everything the guest asked us for: classes found, natives registered, and
// every method/field id it wanted. This is the M4 work list.
void kl_jni_report(FILE *out);

#endif
