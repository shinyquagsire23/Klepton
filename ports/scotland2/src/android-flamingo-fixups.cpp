// Data-only platform implementation of Flamingo's original fixup interfaces.
// The upstream installer still owns metadata, priority and chain management.
#include "fixups.hpp"
#include <cstring>
extern "C" void *sc2_aot_original(const void *);
extern "C" int sc2_aot_exchange(void *,void *,void **);
extern "C" int sc2_aot_restore(void *);
extern "C" int sc2_aot_entry_word(const void *,uint32_t *);

namespace flamingo {
void ShimTarget::WriteJump(void *destination) {
    if (sc2_aot_exchange(addr.data(),destination,nullptr)) {
        FLAMINGO_ABORT("No valid signed dispatch entry for {}",fmt::ptr(addr.data()));
    }
}
void Fixups::CopyOriginalInsts() {
    uint32_t word=0;
    if (sc2_aot_entry_word(target.addr.data(),&word)) {
        FLAMINGO_ABORT("No signed original instruction for {}",fmt::ptr(target.addr.data()));
    }
    original_instructions={word};
    target.original_instructions={word};
}
void Fixups::PerformFixupsAndCallback() {
    if (!sc2_aot_original(target.addr.data()) || fixup_inst_destination.addr.empty()) {
        FLAMINGO_ABORT("Signed original-call trampoline mismatch for {}",fmt::ptr(target.addr.data()));
    }
}
void Fixups::Uninstall() {
    // Restore the dispatch head captured when this Flamingo group was created,
    // including a preceding And64 hook if one was already installed.
    if (sc2_aot_exchange(target.addr.data(),fixup_inst_destination.addr.data(),nullptr)) {
        FLAMINGO_ABORT("Could not restore signed dispatch entry for {}",fmt::ptr(target.addr.data()));
    }
}
void Fixups::Log() const {
    FLAMINGO_DEBUG("Signed trampoline at {}; {} original instruction(s)",fmt::ptr(fixup_inst_destination.addr.data()),original_instructions.size());
}
}
