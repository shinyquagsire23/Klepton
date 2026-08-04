#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#define mach_absolute_time_stub() 0
static inline uint64_t rd(void){uint64_t v;__asm__ volatile("mrs %0, tpidr_el0":"=r"(v));return v;}
static inline uint64_t rro(void){uint64_t v;__asm__ volatile("mrs %0, tpidrro_el0":"=r"(v));return v;}
static inline void wr(uint64_t v){__asm__ volatile("msr tpidr_el0, %0"::"r"(v));}
#define S 0xDEADBEEF12340000ULL
#define CHK(label) do{ uint64_t g=rd(); printf("  %-42s %016llx %s\n", label, \
    (unsigned long long)g, g==S?"[SURVIVED]":"[CLOBBERED]"); }while(0)

int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    volatile uint64_t sink=0;

    printf("== clobber characterisation ==\n");
    wr(S); for(volatile int i=0;i<1000;i++) sink+=i;            CHK("pure compute, no syscall, no call");
    wr(S); sink += (uint64_t)strlen("abcdefgh");                 CHK("libSystem call, no syscall (strlen)");
    wr(S); sink += (uint64_t)getpid();                           CHK("one trivial syscall (getpid)");
    wr(S); sched_yield();                                        CHK("sched_yield");
    wr(S); usleep(2000);                                         CHK("usleep 2ms");
    // guaranteed preemption without any syscall in between
    wr(S); { uint64_t t0=mach_absolute_time_stub(); (void)t0; }   CHK("noop");
    wr(S); for(volatile long i=0;i<80000000L;i++) sink+=i;       CHK("~100ms busy spin, no syscall");

    printf("\n== Darwin TSD layout via TPIDRRO_EL0 ==\n");
    uint64_t tp = rro();
    printf("  pthread_self  = %016llx\n", (unsigned long long)(uintptr_t)pthread_self());
    printf("  TPIDRRO_EL0   = %016llx  (= self + 0x%llx)\n",
        (unsigned long long)tp, (unsigned long long)(tp-(uintptr_t)pthread_self()));
    for (int i=0;i<9;i++)
        printf("    slot %d (+%2d) = %016llx%s\n", i, i*8,
            (unsigned long long)((uint64_t*)tp)[i], i==5?"   <-- bionic TLS_SLOT_STACK_GUARD":"");
    printf("\n  stability of slot 5 across syscalls: ");
    uint64_t a=((uint64_t*)tp)[5]; usleep(3000); sched_yield();
    uint64_t b=((uint64_t*)tp)[5];
    printf("%016llx -> %016llx %s\n",(unsigned long long)a,(unsigned long long)b,a==b?"[STABLE]":"[VARIES]");
    return 0;
}
