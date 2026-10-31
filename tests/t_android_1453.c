// Real native shim behavior needed by the Unity 6.3 guest; no game assets.
#include "klepton.h"
#include "kl_ndk.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/uio.h>
#include <time.h>

static void *(*instance)(void);
static void (*post)(void*,void(*)(long,void*),void*);
static pthread_t owner;
static void *choreographer;
static unsigned calls;
static long previous;
static void callback(long frame,void *data) {
    assert(pthread_equal(owner,pthread_self()));
    assert(data==&calls && frame>previous);previous=frame;
    if(++calls==1)post(choreographer,callback,data);
}
static void *other_thread(void *unused) {
    assert(!instance());kl_ndk_prepare_looper();assert(instance()!=choreographer);
    // Posting from another thread must still run on the owner.
    post(choreographer,callback,&calls);return NULL;
}
int main(void) {
    kl_thread_init();
    instance=kl_shim_lookup("AChoreographer_getInstance");
    post=kl_shim_lookup("AChoreographer_postFrameCallback");assert(instance&&post);
    assert(!instance());kl_ndk_prepare_looper();choreographer=instance();
    assert(choreographer && choreographer==instance());owner=pthread_self();
    post(NULL,callback,&calls);post(choreographer,NULL,&calls);assert(!calls);
    pthread_t thread;assert(!pthread_create(&thread,NULL,other_thread,NULL));pthread_join(thread,NULL);
    for(unsigned i=0;i<20 && calls<2;++i)kl_ndk_pump_looper(30);
    assert(calls==2);for(int i=0;i<3;++i)kl_ndk_pump_looper(20);assert(calls==2);

    ssize_t (*readv_guest)(pid_t,const struct iovec*,unsigned long,const struct iovec*,unsigned long,unsigned long)=kl_shim_lookup("process_vm_readv");
    int *(*guest_errno)(void)=kl_shim_lookup("__errno");assert(readv_guest&&guest_errno);
    const char source[]="abcdef";char dest[8]={0};
    struct iovec local[]={{dest,2},{dest+2,4}},remote[]={{(void*)source,3},{(void*)source+3,3}};
    assert(readv_guest(getpid(),local,2,remote,2,0)==6 && !memcmp(dest,source,6));
    assert(readv_guest(getpid(),NULL,0,NULL,0,0)==0);
    assert(readv_guest(getpid(),local,2,remote,2,1)==-1 && *guest_errno()==22);
    assert(readv_guest(getpid(),local,1025,remote,2,0)==-1 && *guest_errno()==22);
    assert(readv_guest(getpid(),NULL,1,remote,2,0)==-1 && *guest_errno()==14);
    struct iovec bad={(void*)1,1};
    assert(readv_guest(getpid(),local,2,&bad,1,0)==-1 && *guest_errno()==14);
    assert(readv_guest(getpid()+1,local,2,remote,2,0)==-1 && *guest_errno()==1);
    puts("ANDROID_1453_SHIMS_OK owner-thread one-shot/repost frame callbacks and fault-safe self reads; invalid requests rejected.");
}
