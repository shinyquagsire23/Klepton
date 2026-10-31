// Separate Android DSO consumes the actual Flamingo C++ API.
#include "installer.hpp"
#include <cstdint>
#include <variant>
extern "C" int sc2_aot_exchange(void *,void *,void **);

namespace {
uint64_t (*first_original)(uint64_t),(*second_original)(uint64_t),(*final_original)(uint64_t);
uint64_t first(uint64_t x) { return first_original(x)+100; }
uint64_t second(uint64_t x) { return second_original(x)+1000; }
uint64_t last(uint64_t x) { return final_original(x)+10000; }
uint64_t (*outer_original)(uint64_t);
uint64_t outer(uint64_t x) { return outer_original(x)+50000; }
uint32_t wrong_signature(uint32_t x) { return x; }
void *(*game_first_original)(),*(*game_second_original)();
unsigned first_calls,second_calls;
void *game_first() { __atomic_add_fetch(&first_calls,1,__ATOMIC_RELAXED); return game_first_original(); }
void *game_second() { __atomic_add_fetch(&second_calls,1,__ATOMIC_RELAXED); return game_second_original(); }
}
extern "C" __attribute__((visibility("default")))
int sc2_test_flamingo(void *target,void *unprepared) {
    using namespace flamingo;
    auto function=reinterpret_cast<uint64_t (*)(uint64_t)>(target);
    auto baseline=function(0);
    if (baseline!=42 && baseline!=1042) return 1;
    auto one=Install(HookInfo(first,target,&first_original));
    if (!one.has_value() || function(0)!=baseline+100) return 2;
    auto two=Install(HookInfo(second,target,&second_original));
    if (!two.has_value() || function(0)!=baseline+1100) return 3;
    auto three=Install(HookInfo(last,target,&final_original,HookInfo::kDefaultNumInsts,CallingConvention::Cdecl,
        HookNameMetadata{.name="last"},HookPriority{.is_final=true},InstallationMetadata{true,false,false}));
    if (!three.has_value() || function(0)!=baseline+11100) return 4;
    auto conflict=Install(HookInfo(last,target,&final_original,HookInfo::kDefaultNumInsts,CallingConvention::Cdecl,
        HookNameMetadata{.name="conflict"},HookPriority{.is_final=true},InstallationMetadata{true,false,false}));
    if (conflict.has_value() || !std::holds_alternative<installation::TargetBadPriorities>(conflict.error()) || function(0)!=baseline+11100) return 5;
    uint32_t (*wrong_original)(uint32_t)=nullptr;
    auto mismatch=Install(HookInfo(wrong_signature,target,&wrong_original));
    if (mismatch.has_value() || !std::holds_alternative<installation::TargetMismatch>(mismatch.error())) return 6;
    auto metadata=MetadataFor({target});auto fixups=FixupPointerFor({target});
    if (!metadata.has_value() || !fixups.has_value() || OriginalInstsFor({target}).size()!=1 || fixups.value().empty()) return 7;
    auto middle=Uninstall(one.value().returned_handle);
    if (!middle.has_value() || !middle.value() || function(0)!=baseline+11000) return 8;
    auto reinstall=Reinstall({target});
    if (!reinstall.has_value() || !reinstall.value() || function(0)!=baseline+11000) return 9;
    auto head=Uninstall(two.value().returned_handle);
    if (!head.has_value() || !head.value() || function(0)!=baseline+10000) return 10;
    auto tail=Uninstall(three.value().returned_handle);
    if (!tail.has_value() || tail.value() || function(0)!=baseline || MetadataFor({target}).has_value()) return 11;
    auto none=Reinstall({target});
    if (!none.has_value() || none.value()) return 12;
    auto absent=Install(HookInfo(first,unprepared,&first_original));
    if (absent.has_value() || !std::holds_alternative<installation::TargetTooSmall>(absent.error())) return 13;
    auto null=Install(HookInfo(first,nullptr,&first_original));
    if (null.has_value() || !std::holds_alternative<installation::TargetIsNull>(null.error())) return 14;
    auto owned=Install(HookInfo(first,target,&first_original));
    if (!owned.has_value() || sc2_aot_exchange(target,reinterpret_cast<void *>(outer),reinterpret_cast<void **>(&outer_original))) return 15;
    auto interleaved=Install(HookInfo(second,target,&second_original));
    auto blocked=Uninstall(owned.value().returned_handle);
    auto blocked_reinstall=Reinstall({target});
    if (interleaved.has_value() || blocked.has_value() || blocked_reinstall.has_value() || function(0)!=baseline+50100) return 16;
    if (sc2_aot_exchange(target,reinterpret_cast<void *>(outer_original),nullptr)) return 17;
    auto cleanup=Uninstall(owned.value().returned_handle);
    if (!cleanup.has_value() || cleanup.value() || function(0)!=baseline) return 18;
    return 0;
}

extern "C" __attribute__((visibility("default")))
int sc2_flamingo_game_probe(void *target,void *expected) {
    using namespace flamingo;
    auto function=reinterpret_cast<void *(*)()>(target);
    auto first=Install(HookInfo(game_first,target,&game_first_original));
    if (!first.has_value() || function()!=expected || !__atomic_load_n(&first_calls,__ATOMIC_RELAXED)) return 1;
    auto second=Install(HookInfo(game_second,target,&game_second_original));
    if (!second.has_value() || function()!=expected || !__atomic_load_n(&second_calls,__ATOMIC_RELAXED)) return 2;
    auto middle=Uninstall(first.value().returned_handle);
    if (!middle.has_value() || !middle.value() || function()!=expected) return 3;
    auto tail=Uninstall(second.value().returned_handle);
    if (!tail.has_value() || tail.value() || function()!=expected) return 4;
    return 0;
}
