// Exercise the guest-facing JNI slots against temporary files; no game data.
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include "kl_jni.h"
#include "kl_jni_slots.h"
#include "kl_va.h"

#define SLOT(n, name) JNI_##name = n,
enum { KL_JNI_SLOTS(SLOT) };
#undef SLOT
typedef union { uint8_t z; int32_t i; int64_t j; void *l; double d; } jvalue;
static void *env;
static void **jni;
#define JNI(name, type) ((type)jni[JNI_##name])

static void *method(const char *cls, const char *name, const char *sig) {
    return JNI(GetMethodID, void *(*)(void *, void *, const char *, const char *))
        (env, kl_jni_class(cls), name, sig);
}
static void *construct(const char *cls, const char *sig, const jvalue *args) {
    return JNI(NewObjectA, void *(*)(void *, void *, void *, const jvalue *))
        (env, kl_jni_class(cls), method(cls, "<init>", sig), args);
}
static void call(void *stream, const char *name, const char *sig, const jvalue *args) {
    JNI(CallVoidMethodA, void (*)(void *, void *, void *, const jvalue *))
        (env, stream, method("java/io/FileOutputStream", name, sig), args);
}
static void *file_at(const char *path) {
    return construct("java/io/File", "(Ljava/lang/String;)V",
                     (jvalue[]){{.l = kl_jni_new_string(path)}});
}
static int file_bool(void *file, const char *name, const char *sig, const jvalue *args) {
    return JNI(CallBooleanMethodA, uint8_t (*)(void *, void *, void *, const jvalue *))
        (env, file, method("java/io/File", name, sig), args);
}
// C++ guests call the V slots with Android's va_list, which is different from
// this test process's Darwin varargs ABI. Supply an AAPCS64 GP save area.
static kl_va guest_args(uint64_t *values, int count) {
    return (kl_va){.gr_top = values + count, .gr_offs = -8 * count};
}
static void permissions(const char *path, mode_t want) {
    struct stat st;
    assert(stat(path, &st) == 0 && (st.st_mode & 07777) == want);
}
static void exception(const char *cls) {
    void *e = JNI(ExceptionOccurred, void *(*)(void *))(env);
    assert((e != NULL) == (cls != NULL));
    if (!e) return;
    assert(JNI(IsInstanceOf, uint8_t (*)(void *, void *, void *))
        (env, e, kl_jni_class(cls)));
    JNI(ExceptionClear, void (*)(void *))(env);
    JNI(DeleteLocalRef, void (*)(void *, void *))(env, e);
}
static void contents(const char *path, const uint8_t *want, size_t len) {
    uint8_t got[64];
    FILE *f = fopen(path, "rb");
    assert(f);
    size_t count = fread(got, 1, sizeof got, f);
    assert(!ferror(f) && count == len && !memcmp(got, want, len));
    assert(fclose(f) == 0);
}
static int descriptors(void) {
    int count = 0, limit = getdtablesize();
    for (int fd = 0; fd < limit; fd++)
        if (fcntl(fd, F_GETFD) >= 0) count++;
    return count;
}

// The gallery request must complete with Denied through Unity's Java proxy.
static int gallery_callbacks;
static void *gallery_result(void *e,void *cls,int64_t ptr,void *name,void *args) {
    assert(e==env && cls==kl_jni_class("com/unity3d/player/ReflectionHelper") && ptr==42);
    const char *text=JNI(GetStringUTFChars,const char *(*)(void *,void *,uint8_t *))(e,name,NULL);
    assert(strcmp(text,"OnPermissionResult")==0);
    JNI(ReleaseStringUTFChars,void (*)(void *,void *,const char *))(e,name,text);
    assert(JNI(GetArrayLength,int32_t (*)(void *,void *))(e,args)==1);
    void *value=JNI(GetObjectArrayElement,void *(*)(void *,void *,int32_t))(e,args,0);
    assert(JNI(CallIntMethodA,int32_t (*)(void *,void *,void *,const jvalue *))
        (e,value,method("java/lang/Integer","intValue","()I"),NULL)==0);
    gallery_callbacks++;return NULL;
}
static void gallery_permissions(void) {
    void *rh=kl_jni_class("com/unity3d/player/ReflectionHelper");
    kl_jni_method native={"nativeProxyInvoke","(JLjava/lang/String;[Ljava/lang/Object;)Ljava/lang/Object;",(void *)gallery_result};
    assert(JNI(RegisterNatives,int32_t (*)(void *,void *,const kl_jni_method *,int32_t))(env,rh,&native,1)==0);
    void *pm=JNI(GetStaticMethodID,void *(*)(void *,void *,const char *,const char *))
        (env,rh,"newProxyInstance","(Lcom/unity3d/player/UnityPlayer;JLjava/lang/Class;)Ljava/lang/Object;");
    jvalue pa[]={{.l=NULL},{.j=42},{.l=kl_jni_class("com/yasirkula/unity/NativeGalleryPermissionReceiver")}};
    void *proxy=JNI(CallStaticObjectMethodA,void *(*)(void *,void *,void *,const jvalue *))(env,rh,pm,pa);
    const char *sigs[]={
        "(Lcom/unity3d/player/UnityPlayerActivity;Lcom/yasirkula/unity/NativeGalleryPermissionReceiver;ZI)V",
        "(Lcom.unity3d.player.UnityPlayerActivity;Lcom.yasirkula.unity.NativeGalleryPermissionReceiver;ZI)V",
        "(Landroid/content/Context;Lcom/yasirkula/unity/NativeGalleryPermissionReceiver;ZI)V"};
    for(int i=0;i<3;i++) {
        void *cls=kl_jni_class("com/yasirkula/unity/NativeGallery");
        void *m=JNI(GetStaticMethodID,void *(*)(void *,void *,const char *,const char *))(env,cls,"RequestPermission",sigs[i]);
        jvalue args[]={{.l=NULL},{.l=proxy},{.i=1},{.i=0}};
        JNI(CallStaticVoidMethodA,void (*)(void *,void *,void *,const jvalue *))(env,cls,m,args);
        assert(gallery_callbacks==i+1);exception(NULL);
    }
    puts("gallery permission callback: PASS (Denied, each request signature)");
}

int main(void) {
    char root[] = "build/jni-output-XXXXXX", path[1024], missing[1024];
    assert(mkdtemp(root));
    kl_jni_set_files_dir(root);
    env = kl_jni_env(); jni = *(void ***)env;
    kl_jni_local_frame_push();
    gallery_permissions();
    snprintf(path, sizeof path, "%s/output.bin", root);
    FILE *f = fopen(path, "wb");
    assert(f && fwrite("old trailing data", 1, 17, f) == 17);
    assert(fclose(f) == 0);

    void *name = kl_jni_new_string("/sdcard/output.bin");
    void *file = construct("java/io/File", "(Ljava/lang/String;)V", (jvalue[]){{.l = name}});
    void *stream = construct("java/io/FileOutputStream", "(Ljava/io/File;)V",
                             (jvalue[]){{.l = file}});
    assert(stream); exception(NULL);
    struct stat st;
    assert(stat(path, &st) == 0 && st.st_size == 0);

    const uint8_t data[] = {0x10, 0, 0xff, 0x42, 0x80};
    void *bytes = JNI(NewByteArray, void *(*)(void *, int32_t))(env, sizeof data);
    JNI(SetByteArrayRegion, void (*)(void *, void *, int32_t, int32_t, const int8_t *))
        (env, bytes, 0, sizeof data, (const int8_t *)data);
    call(stream, "write", "([BII)V", (jvalue[]){{.l = bytes}, {.i = 1}, {.i = 3}});
    exception(NULL); contents(path, data + 1, 3);
    call(stream, "write", "([BII)V", (jvalue[]){{.l = bytes}, {.i = 5}, {.i = 0}});
    exception(NULL); contents(path, data + 1, 3);
    const int bad[][2] = {{-1, 1}, {0, -1}, {4, 2}, {6, 0}, {INT_MAX, INT_MAX}};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        call(stream, "write", "([BII)V", (jvalue[]){{.l = bytes}, {.i = bad[i][0]}, {.i = bad[i][1]}});
        exception("java/lang/IndexOutOfBoundsException");
    }
    call(stream, "write", "([BII)V", (jvalue[]){{.l = NULL}, {.i = 0}, {.i = 1}});
    exception("java/lang/NullPointerException");
    contents(path, data + 1, 3);
    call(stream, "close", "()V", NULL); exception(NULL);
    call(stream, "close", "()V", NULL); exception(NULL);
    call(stream, "write", "([B)V", (jvalue[]){{.l = bytes}});
    exception("java/io/IOException");

    stream = construct("java/io/FileOutputStream", "(Ljava/lang/String;Z)V",
                       (jvalue[]){{.l = name}, {.z = 1}});
    assert(stream); exception(NULL);
    call(stream, "write", "(I)V", (jvalue[]){{.i = 0x1ff}}); exception(NULL);
    call(stream, "close", "()V", NULL); exception(NULL);
    contents(path, (const uint8_t[]){0, 0xff, 0x42, 0xff}, 4);
    stream = construct("java/io/FileOutputStream", "(Ljava/io/File;Z)V",
                       (jvalue[]){{.l = file}, {.z = 0}});
    assert(stream); exception(NULL);
    call(stream, "write", "([B)V", (jvalue[]){{.l = bytes}}); exception(NULL);
    call(stream, "close", "()V", NULL); exception(NULL);
    contents(path, data, sizeof data);

    // Recycling releases the descriptor even without close(). The runtime
    // quarantines retired objects, so drive it past that grace period first.
    int before = descriptors();
    stream = construct("java/io/FileOutputStream", "(Ljava/lang/String;)V",
                       (jvalue[]){{.l = name}});
    assert(stream); exception(NULL);
    assert(descriptors() == before + 1);
    JNI(DeleteLocalRef, void (*)(void *, void *))(env, stream);
    for (int i = 0; i < 9000; i++) {
        void *unused = kl_jni_new_object("java/lang/Object");
        JNI(DeleteLocalRef, void (*)(void *, void *))(env, unused);
    }
    assert(descriptors() == before);
    assert(!construct("java/io/FileOutputStream", "(Ljava/io/File;)V", (jvalue[]){{.l = NULL}}));
    exception("java/lang/NullPointerException");
    void *badname = kl_jni_new_string("/sdcard/missing/file.bin");
    assert(!construct("java/io/FileOutputStream", "(Ljava/lang/String;)V", (jvalue[]){{.l = badname}}));
    exception("java/io/FileNotFoundException");
    snprintf(missing, sizeof missing, "%s/missing", root);
    assert(stat(missing, &st) != 0 && errno == ENOENT);
    void *dirname = kl_jni_new_string("/sdcard");
    assert(!construct("java/io/FileOutputStream", "(Ljava/lang/String;)V", (jvalue[]){{.l = dirname}}));
    exception("java/io/FileNotFoundException");

    // Force an actual short write followed by EFBIG, with this process's limit.
    stream = construct("java/io/FileOutputStream", "(Ljava/io/File;)V", (jvalue[]){{.l = file}});
    assert(stream); exception(NULL);
    struct rlimit saved, limited;
    assert(getrlimit(RLIMIT_FSIZE, &saved) == 0);
    limited = saved; limited.rlim_cur = 3;
    void (*handler)(int) = signal(SIGXFSZ, SIG_IGN);
    assert(handler != SIG_ERR && setrlimit(RLIMIT_FSIZE, &limited) == 0);
    call(stream, "write", "([B)V", (jvalue[]){{.l = bytes}});
    assert(setrlimit(RLIMIT_FSIZE, &saved) == 0);
    assert(signal(SIGXFSZ, handler) != SIG_ERR);
    exception("java/io/IOException");
    call(stream, "close", "()V", NULL); exception(NULL);
    contents(path, data, 3);

    // Preserve read/execute bits, and distinguish owner-only from everyone.
    assert(chmod(path, 0777) == 0);
    assert(file_bool(file, "setWritable", "(Z)Z", (jvalue[]){{.z = 0}}));
    permissions(path, 0577);
    assert(file_bool(file, "setWritable", "(ZZ)Z", (jvalue[]){{.z = 0}, {.z = 0}}));
    permissions(path, 0555);
    assert(file_bool(file, "setWritable", "(Z)Z", (jvalue[]){{.z = 1}}));
    permissions(path, 0755);
    assert(file_bool(file, "setWritable", "(ZZ)Z", (jvalue[]){{.z = 1}, {.z = 0}}));
    permissions(path, 0777);
    assert(file_bool(file, "setWritable", "(ZZ)Z", (jvalue[]){{.z = 0}, {.z = 1}}));
    permissions(path, 0577);
    assert(file_bool(file, "setReadOnly", "()Z", NULL));
    permissions(path, 0555);
    void *absent = file_at("/sdcard/missing"), *empty = file_at("");
    void *invalid[] = {absent, empty};
    for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        assert(!file_bool(invalid[i], "setWritable", "(Z)Z", (jvalue[]){{.z = 1}}));
        assert(!file_bool(invalid[i], "setReadOnly", "()Z", NULL));
        assert(!file_bool(invalid[i], "delete", "()Z", NULL));
    }
    exception(NULL);

    // Repeat the SDK's cache refresh through the guest JNI V calls it uses:
    // writable -> delete -> open -> read-only -> write -> close. chmod must
    // not prevent a descriptor opened earlier from completing its write.
    for (int i = 0; i < 2; i++) {
        kl_va args = guest_args((uint64_t[]){1}, 1);
        assert(JNI(CallBooleanMethodV, uint8_t (*)(void *, void *, void *, kl_va *))
            (env, file, method("java/io/File", "setWritable", "(Z)Z"), &args));
        args = (kl_va){0};
        assert(JNI(CallBooleanMethodV, uint8_t (*)(void *, void *, void *, kl_va *))
            (env, file, method("java/io/File", "delete", "()Z"), &args));
        assert(stat(path, &st) != 0 && errno == ENOENT);
        args = guest_args((uint64_t[]){(uintptr_t)file}, 1);
        stream = JNI(NewObjectV, void *(*)(void *, void *, void *, kl_va *))
            (env, kl_jni_class("java/io/FileOutputStream"),
             method("java/io/FileOutputStream", "<init>", "(Ljava/io/File;)V"), &args);
        assert(stream); exception(NULL);
        args = (kl_va){0};
        assert(JNI(CallBooleanMethodV, uint8_t (*)(void *, void *, void *, kl_va *))
            (env, file, method("java/io/File", "setReadOnly", "()Z"), &args));
        assert(stat(path, &st) == 0 && !(st.st_mode & 0222));
        args = guest_args((uint64_t[]){(uintptr_t)bytes, 0, sizeof data}, 3);
        JNI(CallVoidMethodV, void (*)(void *, void *, void *, kl_va *))
            (env, stream, method("java/io/FileOutputStream", "write", "([BII)V"),
             &args);
        exception(NULL);
        call(stream, "close", "()V", NULL); exception(NULL);
        contents(path, data, sizeof data);
    }

    // Deleting a symlink preserves its target; nonempty directories survive.
    char link[1024];
    snprintf(link, sizeof link, "%s/link", root);
    assert(symlink("output.bin", link) == 0);
    assert(file_bool(file_at("/sdcard/link"), "delete", "()Z", NULL));
    contents(path, data, sizeof data);
    assert(!file_bool(file_at("/sdcard"), "delete", "()Z", NULL));
    assert(mkdir(missing, 0755) == 0);
    assert(file_bool(absent, "delete", "()Z", NULL));
    assert(!file_bool(absent, "delete", "()Z", NULL));
    assert(file_bool(file, "delete", "()Z", NULL));
    assert(!file_bool(file, "delete", "()Z", NULL));
    exception(NULL);

    kl_jni_local_frame_pop();
    assert(rmdir(root) == 0);
    puts("PASS: JNI file output, permissions, cache refresh, deletion, and failure handling.");
    return 0;
}
