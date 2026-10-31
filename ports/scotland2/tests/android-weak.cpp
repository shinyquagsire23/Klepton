#include <cstdint>
inline __attribute__((visibility("default"))) std::uint64_t sc2_shared_inline=0;
inline __attribute__((visibility("hidden"))) std::uint64_t sc2_private_inline=0;
inline __attribute__((visibility("protected"))) std::uint64_t sc2_protected_inline=0;
struct InlineProbe { InlineProbe(){++sc2_shared_inline;} };
inline __attribute__((visibility("default"))) InlineProbe sc2_once_initialized;
#ifdef SC2_WEAK_SECOND
#define NAME sc2_second_weak_storage
#else
#define NAME sc2_first_weak_storage
#endif
extern "C" __attribute__((visibility("default")))
std::uint64_t *NAME(int kind) {
    if(kind==1)return &sc2_private_inline;
    if(kind==2)return &sc2_protected_inline;
    return &sc2_shared_inline;
}
