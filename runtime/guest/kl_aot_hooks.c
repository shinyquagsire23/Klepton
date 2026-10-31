#include "kl_aot_hooks.h"
#include "kl_env.h"
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char magic[8];
    uint32_t version, entry_size, count, reserved;
    uint64_t elf_shift;
    unsigned char elf_sha256[32];
} aot_header;
typedef struct {
    uint64_t target, original, slot, gate;
    uint32_t patched_word, original_word;
} aot_entry;
_Static_assert(sizeof(aot_header)==64 && sizeof(aot_entry)==40,"hook record ABI changed");
typedef struct module {
    const struct mach_header_64 *mach;
    const aot_header *header;
    const aot_entry *entries;
    unsigned references;
    struct module *next;
} module;
static module *modules;
typedef struct native_gate {
    void *target, *original;
    uintptr_t *slot;
    struct native_gate *next;
} native_gate;
static native_gate *native_gates;
static pthread_mutex_t lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
void kl_aot_begin(void) { pthread_mutex_lock(&lock); }
void kl_aot_end(void) { pthread_mutex_unlock(&lock); }
static _Thread_local char failure[256];
static _Thread_local int failure_code;
static int reject(int code, const char *message) {
    snprintf(failure,sizeof failure,"%s",message); failure_code=code; errno=code; return -1;
}
const char *kl_aot_error(void) { return failure; }
int kl_aot_error_code(void) { return failure_code; }

static int in_segment(const struct mach_header_64 *mh, uint64_t va, uint64_t size, const char *name, int prot) {
    const unsigned char *lc=(const unsigned char *)(mh+1);
    for (uint32_t i=0;i<mh->ncmds;++i) {
        const struct load_command *cmd=(const void *)lc;
        if (cmd->cmd==LC_SEGMENT_64) {
            const struct segment_command_64 *s=(const void *)lc;
            if ((!name || !strncmp(s->segname,name,16)) && s->initprot==prot &&
                va>=s->vmaddr && size<=s->vmsize && va-s->vmaddr<=s->vmsize-size) return 1;
        }
        lc+=cmd->cmdsize;
    }
    return 0;
}
static int executable(const void *pointer) {
    uintptr_t target=(uintptr_t)pointer;
    uint32_t count=_dyld_image_count();
    for (uint32_t i=0;i<count;++i) {
        const struct mach_header_64 *mh=(const void *)_dyld_get_image_header(i);
        if (!mh || mh->magic!=MH_MAGIC_64) continue;
        intptr_t slide=_dyld_get_image_vmaddr_slide(i);
        const unsigned char *lc=(const unsigned char *)(mh+1);
        for (uint32_t j=0;j<mh->ncmds;++j) {
            const struct load_command *cmd=(const void *)lc;
            if (cmd->cmd==LC_SEGMENT_64) {
                const struct segment_command_64 *s=(const void *)lc;
                uintptr_t start=(uintptr_t)(s->vmaddr+slide);
                if ((s->initprot & (VM_PROT_EXECUTE|VM_PROT_WRITE))==VM_PROT_EXECUTE &&
                    target>=start && target-start<s->vmsize) return 1;
            }
            lc+=cmd->cmdsize;
        }
    }
    return 0;
}

static int native_slot_writable(const uintptr_t *slot) {
    uintptr_t target=(uintptr_t)slot;
    for (uint32_t i=0;i<_dyld_image_count();++i) {
        const struct mach_header_64 *mh=(void *)_dyld_get_image_header(i);
        if (!mh || mh->magic!=MH_MAGIC_64) continue;
        intptr_t slide=_dyld_get_image_vmaddr_slide(i);
        const unsigned char *lc=(const void *)(mh+1);
        for (uint32_t j=0;j<mh->ncmds;++j) {
            const struct load_command *cmd=(const void *)lc;
            if (cmd->cmd==LC_SEGMENT_64) {
                const struct segment_command_64 *s=(const void *)lc;
                uintptr_t start=(uintptr_t)(s->vmaddr+slide);
                if (!(s->flags&SG_READ_ONLY) &&
                    strncmp(s->segname,"__DATA_CONST",16) && strncmp(s->segname,"__AUTH_CONST",16) &&
                    (s->initprot&(VM_PROT_WRITE|VM_PROT_EXECUTE))==VM_PROT_WRITE &&
                    s->vmsize>=sizeof *slot && target>=start && target-start<=s->vmsize-sizeof *slot) return 1;
            }
            lc+=cmd->cmdsize;
        }
    }
    return 0;
}
static native_gate *find_native(const void *target) {
    for (native_gate *g=native_gates;g;g=g->next) if (g->target==target) return g;
    return NULL;
}
int kl_aot_register_native_gate(void *target,uintptr_t *slot,void *original) {
    if (!target || !slot || !original || target==original || (uintptr_t)target%4 ||
        (uintptr_t)slot%sizeof *slot || !executable(target) || !executable(original) || !native_slot_writable(slot))
        return reject(EINVAL,"invalid signed native dispatch gate");
    pthread_mutex_lock(&lock);
    native_gate *old=find_native(target);
    if (old) {
        int same=old->slot==slot && old->original==original;
        pthread_mutex_unlock(&lock);
        return same?0:reject(EINVAL,"native gate registration changed");
    }
    for (native_gate *g=native_gates;g;g=g->next) if (g->slot==slot) {
        pthread_mutex_unlock(&lock);return reject(EINVAL,"native gate slot already registered");
    }
    uintptr_t value=__atomic_load_n(slot,__ATOMIC_ACQUIRE);
    if (value && value!=(uintptr_t)original) {
        pthread_mutex_unlock(&lock);return reject(EINVAL,"native gate slot has an unexpected initial value");
    }
    native_gate *g=malloc(sizeof *g);
    if (!g) { pthread_mutex_unlock(&lock);return reject(ENOMEM,"native gate registration allocation failed"); }
    *g=(native_gate){.target=target,.original=original,.slot=slot,.next=native_gates};native_gates=g;
    __atomic_store_n(slot,(uintptr_t)original,__ATOMIC_RELEASE);
    pthread_mutex_unlock(&lock);failure[0]=0;return 0;
}

int kl_aot_attach(const struct mach_header_64 *mh, const void *elf) {
    unsigned long record_size=0, gate_size=0, slot_size=0;
    const aot_header *header=(const void *)getsectiondata(mh,"__SC2HOOK","__records",&record_size);
    const void *gates=getsectiondata(mh,"__SC2HOOK","__gates",&gate_size);
    void *slots=getsectiondata(mh,"__SC2DATA","__slots",&slot_size);
    if (!header && !gates && !slots) return 0;
    if (!header || !gates || !slots || record_size<sizeof *header || memcmp(header->magic,"SC2AOT1",8) ||
        header->version!=1 || header->entry_size!=sizeof(aot_entry) || header->reserved ||
        !header->count || header->count>1000000 ||
        record_size!=sizeof *header+(uint64_t)header->count*sizeof(aot_entry) ||
        gate_size!=(uint64_t)header->count*48 || slot_size!=(uint64_t)header->count*8 ||
        (uintptr_t)elf-(uintptr_t)mh!=header->elf_shift)
        return reject(EINVAL,"invalid or incompatible signed hook metadata");
    const aot_entry *entries=(const void *)(header+1);
    uintptr_t base=(uintptr_t)mh;
    for (uint32_t i=0;i<header->count;++i) {
        const aot_entry *e=entries+i;
        if (e->target%4 || (i && entries[i-1].target>=e->target) ||
            e->original!=e->gate+16 || base+e->gate!=(uintptr_t)gates+(uint64_t)i*48 ||
            base+e->slot!=(uintptr_t)slots+(uint64_t)i*8 ||
            !in_segment(mh,e->target,4,NULL,VM_PROT_READ|VM_PROT_EXECUTE) ||
            !in_segment(mh,e->gate,48,"__SC2HOOK",VM_PROT_READ|VM_PROT_EXECUTE) ||
            !in_segment(mh,e->slot,8,"__SC2DATA",VM_PROT_READ|VM_PROT_WRITE))
            return reject(EINVAL,"signed hook record outside its permitted segments");
        int64_t delta=(int64_t)e->gate-(int64_t)e->target;
        if (delta%4 || delta<-(1LL<<27) || delta>=(1LL<<27) ||
            e->patched_word!=(0x14000000u|((uint32_t)(delta/4)&0x3ffffff)) ||
            *(const uint32_t *)(base+e->target)!=e->patched_word)
            return reject(EINVAL,"signed hook entry branch mismatch");
        const uint32_t *gate=(const void *)(base+e->gate);
        int64_t page_delta=(int64_t)(e->slot/4096)-(int64_t)(e->gate/4096);
        uint32_t imm=(uint32_t)page_delta&0x1fffff;
        uint32_t expected_adrp=0x90000010u|((imm&3)<<29)|((imm>>2)<<5);
        if (page_delta<-(1<<20) || page_delta>=(1<<20) || gate[0]!=expected_adrp ||
            gate[1]!=(0x91000210u|((uint32_t)(e->slot&4095)<<10)) ||
            gate[2]!=0xc8dffe10u || gate[3]!=0xd61f0200u)
            return reject(EINVAL,"signed hook gate instruction mismatch");
    }
    pthread_mutex_lock(&lock);
    for (module *m=modules;m;m=m->next) if (m->mach==mh) {
        ++m->references; pthread_mutex_unlock(&lock); return 0;
    }
    module *m=calloc(1,sizeof *m);
    if (!m) { pthread_mutex_unlock(&lock); return reject(ENOMEM,"hook registry allocation failed"); }
    for (uint32_t i=0;i<header->count;++i) {
        uintptr_t *slot=(void *)(base+entries[i].slot);
        uintptr_t value=__atomic_load_n(slot,__ATOMIC_RELAXED);
        // dyld can retain a mapping after the last Klepton owner unloads it.
        // Detach leaves that mapping callable through its signed original.
        if (value && value!=base+entries[i].original) {
            free(m); pthread_mutex_unlock(&lock); return reject(EINVAL,"unregistered hook slot was not zero initialized");
        }
    }
    for (uint32_t i=0;i<header->count;++i)
        __atomic_store_n((uintptr_t *)(base+entries[i].slot),base+entries[i].original,__ATOMIC_RELEASE);
    *m=(module){.mach=mh,.header=header,.entries=entries,.references=1,.next=modules}; modules=m;
    pthread_mutex_unlock(&lock);
    if (kl_env_on("KL_TRACE_AOT",0)) fprintf(stderr,"[scotland2] AOT_READY %u precompiled hook entries; executable pages unchanged\n",header->count);
    return 0;
}
void kl_aot_detach(const struct mach_header_64 *mh) {
    pthread_mutex_lock(&lock);
    module **p=&modules;
    while (*p) {
        module *m=*p;
        if (m->mach==mh) {
            if (--m->references==0) {
                for (uint32_t i=0;i<m->header->count;++i)
                    __atomic_store_n((uintptr_t *)((uintptr_t)mh+m->entries[i].slot),
                        (uintptr_t)mh+m->entries[i].original,__ATOMIC_RELEASE);
                *p=m->next; free(m);
            }
            break;
        }
        p=&m->next;
    }
    pthread_mutex_unlock(&lock);
}
static const aot_entry *find(const void *target, uintptr_t *base) {
    for (module *m=modules;m;m=m->next) {
        uintptr_t address=(uintptr_t)target-(uintptr_t)m->mach;
        uint32_t low=0,high=m->header->count;
        while (low<high) {
            uint32_t mid=low+(high-low)/2;
            if (m->entries[mid].target<address) low=mid+1; else high=mid;
        }
        if (low<m->header->count && m->entries[low].target==address) {
            *base=(uintptr_t)m->mach; return m->entries+low;
        }
    }
    return NULL;
}
void *kl_aot_original(const void *target) {
    uintptr_t base=0;
    pthread_mutex_lock(&lock);
    const aot_entry *e=find(target,&base);
    native_gate *g=find_native(target);
    void *result=g?g->original:e?(void *)(base+e->original):NULL;
    pthread_mutex_unlock(&lock);
    if (!result) reject(ENOENT,"target has no precompiled hook entry");
    return result;
}
size_t kl_aot_copy_code(const void *source,void *output,size_t size) {
    uintptr_t start=(uintptr_t)source;size_t count=0;
    pthread_mutex_lock(&lock);
    if (output && size) memcpy(output,source,size);
    if (size>=4) for (module *m=modules;m;m=m->next)
        for (uint32_t i=0;i<m->header->count;++i) {
            uintptr_t target=(uintptr_t)m->mach+m->entries[i].target;
            if (target>=start && target-start<=size-4) {
                ++count;
                if (output) memcpy((char *)output+(target-start),&m->entries[i].original_word,4);
            }
        }
    pthread_mutex_unlock(&lock);return count;
}
int kl_aot_entry_word(const void *target,uint32_t *word) {
    if (!word) return reject(EINVAL,"original instruction output is null");
    uintptr_t base=0;
    pthread_mutex_lock(&lock);
    const aot_entry *e=find(target,&base);
    if (e) *word=e->original_word;
    native_gate *g=find_native(target);
    if (g) memcpy(word,target,sizeof *word); // native dispatch text is never changed
    pthread_mutex_unlock(&lock);
    return (e||g)?0:reject(ENOENT,"target has no precompiled hook entry");
}
void *kl_aot_current(const void *target) {
    uintptr_t base=0;
    pthread_mutex_lock(&lock);
    const aot_entry *e=find(target,&base);
    native_gate *g=find_native(target);
    uintptr_t *slot=g?g->slot:e?(uintptr_t *)(base+e->slot):NULL;
    void *result=slot?(void *)__atomic_load_n(slot,__ATOMIC_ACQUIRE):NULL;
    pthread_mutex_unlock(&lock);
    if (!result) reject(ENOENT,"target has no precompiled hook entry");
    return result;
}
int kl_aot_exchange(void *target, void *replacement, void **previous) {
    if (previous) *previous=NULL;
    if (!target || !replacement || (uintptr_t)replacement%4) return reject(EINVAL,"invalid hook target or replacement");
    pthread_mutex_lock(&lock);
    uintptr_t base=0;
    const aot_entry *e=find(target,&base);
    native_gate *g=find_native(target);
    if (!e && !g) { pthread_mutex_unlock(&lock); return reject(ENOENT,"target has no precompiled hook entry"); }
    if (target==replacement || !executable(replacement)) {
        pthread_mutex_unlock(&lock); return reject(EINVAL,"replacement is recursive or outside signed executable images");
    }
    uintptr_t *slot=g?g->slot:(uintptr_t *)(base+e->slot);
    uintptr_t old=__atomic_load_n(slot,__ATOMIC_ACQUIRE);
    if (previous) *previous=(void *)old;
    // Publish the caller's original pointer before another thread can enter
    // the replacement and use it. Gate readers use LDAR.
    __atomic_store_n(slot,(uintptr_t)replacement,__ATOMIC_RELEASE);
    pthread_mutex_unlock(&lock); failure[0]=0; return 0;
}
int kl_aot_restore(void *target) {
    pthread_mutex_lock(&lock);
    uintptr_t base=0;
    const aot_entry *e=find(target,&base);
    native_gate *g=find_native(target);
    if (!e && !g) { pthread_mutex_unlock(&lock); return reject(ENOENT,"target has no precompiled hook entry"); }
    __atomic_store_n(g?g->slot:(uintptr_t *)(base+e->slot),g?(uintptr_t)g->original:base+e->original,__ATOMIC_RELEASE);
    pthread_mutex_unlock(&lock); failure[0]=0; return 0;
}
