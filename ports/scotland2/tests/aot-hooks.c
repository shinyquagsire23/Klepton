#include "klepton.h"
#include "kl_aot_hooks.h"
#include <assert.h>
#include <errno.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint64_t (*sum_fn)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
static uint64_t native_sum(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f,uint64_t g,uint64_t h,uint64_t i,uint64_t j) {
    return a+b+c+d+e+f+g+h+i+j;
}
static uintptr_t native_dispatch=(uintptr_t)native_sum;
static const uintptr_t immutable_dispatch=(uintptr_t)native_sum;
static uint64_t native_gate_sum(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f,uint64_t g,uint64_t h,uint64_t i,uint64_t j) {
    return ((sum_fn)__atomic_load_n(&native_dispatch,__ATOMIC_ACQUIRE))(a,b,c,d,e,f,g,h,i,j);
}
static sum_fn sum_orig, sum_previous;
static sum_fn bottom_original;
static uint64_t beneath(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f,uint64_t g,uint64_t h,uint64_t i,uint64_t j) {
    return bottom_original(a,b,c,d,e,f,g,h,i,j)+10;
}
static uint64_t first(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f,uint64_t g,uint64_t h,uint64_t i,uint64_t j) {
    return sum_orig(a,b,c,d,e,f,g,h,i,j)+100;
}
static uint64_t second(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f,uint64_t g,uint64_t h,uint64_t i,uint64_t j) {
    return sum_previous(a,b,c,d,e,f,g,h,i,j)+1000;
}
typedef struct { uint64_t a,b,c,d; } big;
static big (*big_orig)(uint64_t,uint64_t,uint64_t,uint64_t);
static big big_hook(uint64_t a,uint64_t b,uint64_t c,uint64_t d) {
    big result=big_orig(a,b,c,d); result.d+=77; return result;
}
static double (*double_orig)(double,double,double,double);
static double double_hook(double a,double b,double c,double d) { return double_orig(a,b,c,d)*2; }
static atomic_int stop, bad;
static sum_fn target_sum;
static void *worker(void *arg) {
    (void)arg; kl_thread_init();
    while (!atomic_load(&stop)) {
        uint64_t result=target_sum(1,2,3,4,5,6,7,8,9,10);
        if (result!=55 && result!=155) atomic_store(&bad,1);
    }
    return NULL;
}
// Set Z immediately before a real indirect call, preserving the caller's LR.
static uint64_t conditional(void *function, int equal) {
    uint64_t result;
    __asm__ volatile("stp x29,x30,[sp,#-16]!\n"
                     "cmp %w[flag],#1\n"
                     "blr %[fn]\n"
                     "mov %[result],x0\n"
                     "ldp x29,x30,[sp],#16"
        : [result]"=r"(result) : [flag]"r"(equal),[fn]"r"(function)
        : "x0","x1","x2","x3","x4","x5","x6","x7","x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","cc","memory");
    return result;
}
int main(int argc,char **argv) {
    assert(argc==2); kl_thread_init();
    unsigned char native_code[32];memcpy(native_code,native_gate_sum,sizeof native_code);
    uintptr_t stack_slot=(uintptr_t)native_sum;
    assert(kl_aot_register_native_gate(native_gate_sum,&stack_slot,native_sum)==-1);
    assert(kl_aot_register_native_gate(native_gate_sum,(uintptr_t *)&immutable_dispatch,native_sum)==-1);
    assert(!kl_aot_register_native_gate(native_gate_sum,&native_dispatch,native_sum));
    assert(kl_aot_original(native_gate_sum)==native_sum && kl_aot_current(native_gate_sum)==native_sum);
    sum_orig=native_sum;
    assert(!kl_aot_exchange(native_gate_sum,first,(void **)&sum_previous) && sum_previous==native_sum);
    assert(native_gate_sum(1,2,3,4,5,6,7,8,9,10)==155);
    assert(!kl_aot_register_native_gate(native_gate_sum,&native_dispatch,native_sum)); // idempotent, preserves hook
    assert(native_gate_sum(1,2,3,4,5,6,7,8,9,10)==155);
    assert(!kl_aot_exchange(native_gate_sum,second,(void **)&sum_previous) && sum_previous==first);
    assert(native_gate_sum(1,2,3,4,5,6,7,8,9,10)==1155);
    assert(!kl_aot_restore(native_gate_sum) && native_gate_sum(1,2,3,4,5,6,7,8,9,10)==55);
    assert(!memcmp(native_code,native_gate_sum,sizeof native_code));
    puts("NATIVE_SIGNED_GATE_OK native dispatcher hooks chain and restore through writable data; signed text unchanged; stack/const slots rejected.");
    kl_image *image=kl_load_auto(argv[1]);
    if (!image) { fprintf(stderr,"%s\n",kl_error()); return 1; }
    assert(!kl_get_stats(image)->imports_missing);
    const char *names[]={"sc2_sum10","sc2_double","sc2_big","sc2_literal","sc2_adr","sc2_adrp","sc2_cbz","sc2_tbz","sc2_bcond","sc2_tail"};
    void *functions[10]; unsigned char entries[10][4],gates[10][48];
    for (int i=0;i<10;++i) {
        functions[i]=kl_sym(image,names[i]); assert(functions[i]);
        void *original=kl_aot_original(functions[i]); assert(original);
        memcpy(entries[i],functions[i],4); memcpy(gates[i],(char *)original-16,48);
    }
    target_sum=functions[0];
    assert(target_sum(1,2,3,4,5,6,7,8,9,10)==55);
    sum_orig=kl_aot_original(functions[0]);
    assert(kl_aot_exchange(functions[0],first,(void **)&sum_previous)==0 && sum_previous==sum_orig);
    assert(target_sum(1,2,3,4,5,6,7,8,9,10)==155);
    assert(kl_aot_exchange(functions[0],second,(void **)&sum_previous)==0 && sum_previous==first);
    assert(target_sum(1,2,3,4,5,6,7,8,9,10)==1155);
    assert(kl_aot_restore(functions[0])==0 && target_sum(1,2,3,4,5,6,7,8,9,10)==55);
    assert(kl_aot_original(sum_orig));
    assert(!kl_aot_exchange(functions[0],first,NULL));
    assert(!kl_aot_exchange(sum_orig,beneath,(void **)&bottom_original));
    assert(target_sum(1,2,3,4,5,6,7,8,9,10)==165);
    assert(!kl_aot_restore(sum_orig));
    assert(target_sum(1,2,3,4,5,6,7,8,9,10)==155);
    assert(!kl_aot_restore(functions[0]));
    double_orig=kl_aot_original(functions[1]);
    double (*fp)(double,double,double,double)=functions[1];
    assert(fp(1.25,2.5,3.75,4.5)==12.0);
    assert(kl_aot_exchange(functions[1],double_hook,NULL)==0 && fp(1.25,2.5,3.75,4.5)==24.0);
    big_orig=kl_aot_original(functions[2]);
    big (*large)(uint64_t,uint64_t,uint64_t,uint64_t)=functions[2];
    assert(kl_aot_exchange(functions[2],big_hook,NULL)==0);
    big result=large(5,6,7,8); assert(result.a==5 && result.b==6 && result.c==7 && result.d==85);
    assert(((uint64_t (*)(void))functions[3])()==1234567);
    assert(!strcmp(((const char *(*)(void))functions[4])(),"signed-original-address"));
    assert(((uint64_t (*)(void))functions[5])()==7654321);
    uint64_t (*cbz)(uint64_t)=functions[6],(*tbz)(uint64_t)=functions[7];
    assert(cbz(0)==42 && cbz(9)==10 && tbz(0)==11 && tbz(4)==99);
    assert(conditional(functions[8],1)==17 && conditional(functions[8],0)==13);
    assert(((uint64_t (*)(void))functions[9])()==321);
    void *load_immediate=kl_sym(image,"sc2_load_immediate");
    uint64_t load_data[19]={0};load_data[18]=0x123456789abcdef0ULL;
    assert(load_immediate && kl_aot_original(load_immediate));
    assert(((uint64_t (*)(void*))load_immediate)(load_data)==load_data[18]);
    assert(((uint64_t (*)(void*))kl_aot_original(kl_aot_original(load_immediate)))(load_data)==load_data[18]);
    void *unprepared=kl_sym(image,"sc2_unprepared");
    assert(unprepared && !kl_aot_original(unprepared));
    assert(kl_aot_exchange(unprepared,first,NULL)==-1 && errno==ENOENT);
    assert(kl_aot_exchange(functions[0],functions[0],NULL)==-1);
    uint64_t data=0;
    assert(kl_aot_exchange(functions[0],&data,NULL)==-1);
    pthread_t threads[3];
    for (int i=0;i<3;++i) assert(!pthread_create(threads+i,NULL,worker,NULL));
    for (int i=0;i<1000;++i) {
        assert(!kl_aot_exchange(functions[0],first,NULL));
        assert(!kl_aot_restore(functions[0]));
    }
    atomic_store(&stop,1);
    for (int i=0;i<3;++i) pthread_join(threads[i],NULL);
    assert(!atomic_load(&bad));
    for (int i=0;i<10;++i) {
        void *original=kl_aot_original(functions[i]);
        assert(!memcmp(entries[i],functions[i],4) && !memcmp(gates[i],(char *)original-16,48));
    }
    Dl_info loaded;
    assert(dladdr(functions[0],&loaded) && loaded.dli_fname);
    void *retained=dlopen(loaded.dli_fname,RTLD_NOW|RTLD_LOCAL);
    assert(retained);
    assert(!kl_aot_exchange(functions[0],first,NULL));
    kl_unload(image);
    assert(!kl_aot_original(functions[0]));
    assert(target_sum(1,2,3,4,5,6,7,8,9,10)==55);
    image=kl_load_auto(argv[1]); assert(image);
    assert(kl_aot_original(kl_sym(image,names[0])));
    assert(((sum_fn)kl_sym(image,names[0]))(1,2,3,4,5,6,7,8,9,10)==55);
    kl_unload(image); dlclose(retained);
    puts("PASS: signed ARM64 hooks, original calls, chain/restore, integer/stack/FP/structure-return ABI, PC-relative relocation, concurrent dispatch and unchanged executable bytes.");
    return 0;
}
