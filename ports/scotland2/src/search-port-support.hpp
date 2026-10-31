#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Decode only the JWT payload used to read user-id/expiry claims. Authentication
// remains the original Airbuds HTTPS request; this is not signature verification.
namespace sc2_search {
inline std::string base64_url_decode(std::string_view input) {
    if (input.empty() || input.size() > 16384) return {};
    const auto original_size = input.size();
    std::size_t padding = 0;
    while (!input.empty() && input.back() == '=') {
        input.remove_suffix(1);
        if (++padding > 2) return {};
    }
    if (input.size() % 4 == 1 || (padding &&
        (original_size % 4 || padding != (4 - input.size() % 4) % 4))) return {};
    std::string output;
    output.reserve(input.size() * 3 / 4);
    std::uint32_t buffer = 0;
    unsigned bits = 0;
    for (const unsigned char ch : input) {
        unsigned value;
        if (ch >= 'A' && ch <= 'Z') value = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') value = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') value = ch - '0' + 52;
        else if (ch == '-' || ch == '+') value = 62;
        else if (ch == '_' || ch == '/') value = 63;
        else return {};
        buffer = (buffer << 6) | value;
        bits += 6;
        if (bits >= 8) { bits -= 8; output.push_back(static_cast<char>((buffer >> bits) & 255)); }
    }
    if (bits && (buffer & ((1u << bits) - 1))) return {};
    return output;
}
}

#ifdef SC2_AOT_HOOKS
struct SC2SearchHookTarget {
    std::uint64_t address;
    std::uint32_t originalHook, reserved;
    char name[128];
};
static_assert(sizeof(SC2SearchHookTarget) == 144);
#define SC2_RECORD_SEARCH_HOOK(name_, method_) \
    static const SC2SearchHookTarget sc2_search_target_##name_ __attribute__((used,section(".sc2_hooks"))) = { \
        ::il2cpp_utils::il2cpp_type_check::MetadataGetter<method_>::addrs, 0, 0, #name_ };
#else
#define SC2_RECORD_SEARCH_HOOK(name_, method_)
#endif
