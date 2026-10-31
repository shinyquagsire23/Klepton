// Preserve actual Capstone while exposing prepared entry words to SDK code
// inspectors at their original virtual PCs. The executable mapping is untouched.
#include "capstone/shared/capstone/capstone.h"
#include <cstdlib>
#include <cstdint>
extern "C" size_t sc2_aot_copy_code(const void *,void *,size_t);
extern "C" bool sc2_capstone_iter_raw(csh,const uint8_t **,size_t *,uint64_t *,cs_insn *);
extern "C" size_t sc2_capstone_disasm_raw(csh,const uint8_t *,size_t,uint64_t,size_t,cs_insn **);
extern "C" __attribute__((visibility("default")))
bool cs_disasm_iter(csh handle,const uint8_t **code,size_t *size,uint64_t *address,cs_insn *instruction) {
    if (!code || !size || *size<4 || !sc2_aot_copy_code(*code,nullptr,4))
        return sc2_capstone_iter_raw(handle,code,size,address,instruction);
    uint32_t word;sc2_aot_copy_code(*code,&word,4);
    const uint8_t *input=reinterpret_cast<const uint8_t *>(&word);size_t remaining=4;
    bool result=sc2_capstone_iter_raw(handle,&input,&remaining,address,instruction);
    if (result) { *code+=4-remaining; *size-=4-remaining; }
    return result;
}
extern "C" __attribute__((visibility("default")))
size_t cs_disasm(csh handle,const uint8_t *code,size_t size,uint64_t address,size_t count,cs_insn **instructions) {
    if (!sc2_aot_copy_code(code,nullptr,size))
        return sc2_capstone_disasm_raw(handle,code,size,address,count,instructions);
    void *copy=std::malloc(size);
    if (!copy) { if (instructions) *instructions=nullptr; return 0; }
    sc2_aot_copy_code(code,copy,size);
    size_t result=sc2_capstone_disasm_raw(handle,static_cast<const uint8_t *>(copy),size,address,count,instructions);
    std::free(copy);return result;
}
