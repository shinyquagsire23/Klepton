#include <cstdint>
#include "sc2-customtypes-target.h"
extern "C" int sc2_aot_entry_word(const void *,uint32_t *);
namespace custom_types {
int sc2_check_customtypes_unity_target(uintptr_t address) {
    uint32_t word=0;
    return !sc2_aot_entry_word(reinterpret_cast<void *>(address),&word) && word==SC2_CUSTOMTYPES_SCRIPTING_CLASS_WORD;
}
}
