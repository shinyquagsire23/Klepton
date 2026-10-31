// Actual wake counts matter to Rust's futex reader/writer lock.
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
long klb_syscall(long,long,long,long,long,long,long);
enum { WAIT=0,WAKE=1,WAKE_OP=5,WAIT_BITSET=9,WAKE_BITSET=10 };
static long futex(int32_t *p,int op,uint32_t value,const void *arg,int32_t *p2,uint32_t v3) {
    return klb_syscall(98,(long)p,op|128,value,(long)arg,(long)p2,v3);
}
typedef struct { int32_t *word; uint32_t mask; atomic_int ready; long result; } job;
static void *waiter(void *arg) {
    job *j=arg;struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC,&deadline);deadline.tv_sec+=3;
    atomic_store(&j->ready,1);
    j->result=futex(j->word,WAIT_BITSET,0,&deadline,NULL,j->mask);
    return NULL;
}
static long wake_eventually(int32_t *word,int op,uint32_t mask) {
    for (int i=0;i<1000;++i) {
        long n=futex(word,op,1,NULL,NULL,mask);
        assert(n==0 || n==1);
        if (n) return n;
        usleep(1000);
    }
    assert(!"waiter failed to become reachable");return -1;
}
int main(void) {
    int32_t word=0,other=0;
    assert(futex(&word,WAKE,INT_MAX,NULL,NULL,0)==0);
    assert(futex(&word,WAIT,1,NULL,NULL,0)==-1 && errno==EAGAIN);
    assert(futex(&word,WAIT_BITSET,0,NULL,NULL,0)==-1 && errno==EINVAL);
    struct timespec short_wait={.tv_nsec=1000000};
    assert(futex(&word,WAIT,0,&short_wait,NULL,0)==-1 && errno==ETIMEDOUT);
    assert(futex(&word,WAKE,1,NULL,NULL,0)==0);
    job jobs[2]={{.word=&word,.mask=1},{.word=&word,.mask=2}};pthread_t threads[2];
    for (int i=0;i<2;++i) assert(!pthread_create(threads+i,NULL,waiter,jobs+i));
    while (!atomic_load(&jobs[0].ready) || !atomic_load(&jobs[1].ready)) usleep(100);
    assert(futex(&other,WAKE,INT_MAX,NULL,NULL,0)==0);
    assert(wake_eventually(&word,WAKE_BITSET,1)==1);
    pthread_join(threads[0],NULL);assert(jobs[0].result==0);
    assert(futex(&word,WAKE_BITSET,INT_MAX,NULL,NULL,1)==0);
    // ADD 0; compare old word with 99: false, so the second waiter stays asleep.
    uint32_t operation=(1u<<28)|99u;
    assert(futex(&other,WAKE_OP,1,(void *)1,&word,operation)==0 && word==0);
    assert(wake_eventually(&word,WAKE_BITSET,2)==1);
    pthread_join(threads[1],NULL);assert(jobs[1].result==0);
    assert(futex(&word,WAKE,INT_MAX,NULL,NULL,0)==0);
    job third={.word=&word,.mask=4};pthread_t t;
    assert(!pthread_create(&t,NULL,waiter,&third));
    while (!atomic_load(&third.ready)) usleep(100);
    // OR 0 and compare with 0: the conditional second wake must report one.
    operation=2u<<28;
    long count=0;
    for (int i=0;i<1000 && !count;++i) {
        count=futex(&other,WAKE_OP,1,(void *)1,&word,operation);
        if (!count) usleep(1000);
    }
    assert(count==1);pthread_join(t,NULL);assert(third.result==0);
    // Signed immediate -1, compare previous value 1: true, no phantom wake.
    word=1;
    operation=(1u<<28)|(0xfffu<<12)|1u;
    assert(futex(&other,WAKE_OP,1,(void *)1,&word,operation)==0 && word==0);
    errno=0;assert(futex(NULL,WAIT,0,NULL,NULL,0)==-1 && errno==EFAULT);
    errno=0;assert(futex((int32_t*)((char*)&word+1),WAIT,0,NULL,NULL,0)==-1 && errno==EINVAL);
    errno=0;assert(futex(&word,WAKE,UINT32_MAX,NULL,NULL,0)==-1 && errno==EINVAL);
    struct timespec excessive={.tv_sec=INT64_MAX,.tv_nsec=999999999};word=0;
    errno=0;assert(futex(&word,WAIT,0,&excessive,NULL,0)==-1 && errno==EINVAL);
    // A pending cancellation must not leave a stack waiter or locked bucket.
    job canceled={.word=&word,.mask=8};pthread_t ct;
    assert(!pthread_create(&ct,NULL,waiter,&canceled));
    while(!atomic_load(&canceled.ready))usleep(100);
    usleep(10000);assert(!pthread_cancel(ct));
    assert(wake_eventually(&word,WAKE_BITSET,8)==1);pthread_join(ct,NULL);
    assert(futex(&word,WAKE,INT_MAX,NULL,NULL,0)==0);
    puts("PASS: futex actual/zero wake counts, mask/address isolation, timeout removal and conditional atomic wake operations.");
    return 0;
}
