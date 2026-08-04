// Validate the proposed fix: guest reads TLS via TPIDRRO_EL0 + bionic slot offsets
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#define TLS_SLOT_STACK_GUARD 5
static inline uint64_t rro(void){uint64_t v;__asm__ volatile("mrs %0, tpidrro_el0":"=r"(v));return v;}
// exactly what a rewritten guest stack-protector prologue would execute
static inline uint64_t guest_read_canary(void){
    uint64_t v; __asm__ volatile("mrs %0, tpidrro_el0\n\tldr %0, [%0, #40]":"=r"(v)); return v;
}
static void klepton_thread_init(uint64_t canary){ ((uint64_t*)rro())[TLS_SLOT_STACK_GUARD] = canary; }

static void *worker(void *arg){
    uint64_t want = (uint64_t)(uintptr_t)arg;
    klepton_thread_init(want);
    long bad=0; volatile long sink=0;
    for (int round=0; round<40; round++){
        for(volatile long i=0;i<3000000L;i++) sink+=i;   // force preemption
        usleep(500);                                      // force context switch
        if (guest_read_canary() != want) bad++;
    }
    printf("  thread canary=%016llx  40 rounds across preemption+sleep -> %ld mismatches %s\n",
        (unsigned long long)want, bad, bad? "[FAIL]":"[OK]");
    return NULL;
}
int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    printf("== proposed fix: TPIDRRO_EL0 + bionic slot 5 ==\n");
    pthread_t t[4];
    for (int i=0;i<4;i++) pthread_create(&t[i],NULL,worker,(void*)(uintptr_t)(0xC0FFEE0000ULL+i));
    for (int i=0;i<4;i++) pthread_join(t[i],NULL);
    printf("\n  slot 5 still free after Foundation-less run: %s\n",
        ((uint64_t*)rro())[5]==0 ? "yes (main thread untouched)" : "written by us or system");
    return 0;
}
