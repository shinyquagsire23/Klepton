#pragma once

#include "beatsaber-hook/shared/utils/capstone-utils.hpp"
#include "logging.hpp"

namespace cs {
    template<class T, uint32_t nToRetOn, auto match, auto skip, int retCount = -1, size_t szBytes = 4096>
    requires ((nToRetOn >= 1 && (szBytes % 4) == 0))
    T findNthSafe(const uint32_t* addr) {
        cs_insn* insn = cs_malloc(getHandle());
        auto ptr = reinterpret_cast<uint64_t>(addr);
        auto instructions = reinterpret_cast<const uint8_t*>(addr);

        int rCount = retCount;
        uint32_t nCalls = nToRetOn;
        size_t sz = szBytes;
        while (sz > 0) {
            bool res = cs_disasm_iter(getHandle(), &instructions, &sz, &ptr, insn);
            custom_types::logger.debug("0x{:08x} diassemb: {} (rCount: {}, nCalls: {}, sz: {})", ptr, insn->mnemonic, rCount, nCalls, sz);
            if (res) {
                // Valid decode, so lets check to see if it is a match or we need to break.
                if (insn->id == ARM64_INS_RET) {
                    if (rCount == 0) {
                        // Early termination!
                        cs_free(insn, 1);
                        custom_types::logger.debug("Could not find: {} call at: {} within: {} rets! Found all of the rets first!", nToRetOn, fmt::ptr(addr), retCount);
                        return {};
                    }
                    rCount--;
                } else {
                    auto testRes = match(insn);
                    if (testRes) {
                        if (nCalls == 1) {
                            cs_free(insn, 1);
                            return testRes;
                        } else {
                            nCalls--;
                        }
                    } else if (skip(insn)) {
                        if (nCalls == 1) {
                            std::string name(insn->mnemonic);
                            cs_free(insn, 1);
                            custom_types::logger.debug("Found: {} match, at: {} within: {} rets, but the result was a {}! Cannot compute destination address!", nToRetOn, fmt::ptr(addr), retCount, name.c_str());
                            return {};
                        } else {
                            nCalls--;
                        }
                    }
                }
                // Other instructions are ignored silently
            }
            else {
                // Invalid instructions are ignored silently.
                // In order to skip these properly, we must increment our instructions, ptr, and size accordingly.
                custom_types::logger.debug("FAILED PARSE: 0x{:08x} diassemb: 0x{:08x}", ptr, *(uint32_t*)ptr);
                sz -= 4;
                ptr += 4;
                instructions += 4;
            }
        }
        // If we run out of bytes to parse, we fail
        cs_free(insn, 1);
        custom_types::logger.debug("Could not find: {} call at: {} within: {} rets, within size: {}!", nToRetOn, fmt::ptr(addr), retCount, szBytes);
        return {};
    }

    template<uint32_t nToRetOn, bool includeR = false, int retCount = -1, size_t szBytes = 4096>
    requires ((nToRetOn >= 1 && (szBytes % 4) == 0))
    auto findNthBlSafe(const uint32_t* addr) {
        if constexpr (includeR) {
            return findNthSafe<std::optional<uint32_t*>, nToRetOn, &blConv, &insnMatch<ARM64_INS_BLR>, retCount, szBytes>(addr);
        } else {
            return findNthSafe<std::optional<uint32_t*>, nToRetOn, &blConv, &insnMatch<>, retCount, szBytes>(addr);
        }
    }

    template<uint32_t nToRetOn, bool includeR = false, int retCount = -1, size_t szBytes = 4096>
    requires ((nToRetOn >= 1 && (szBytes % 4) == 0))
    auto findNthBSafe(const uint32_t* addr) {
        if constexpr (includeR) {
            return findNthSafe<std::optional<uint32_t*>, nToRetOn, &bConv, &insnMatch<ARM64_INS_BR>, retCount, szBytes>(addr);
        } else {
            return findNthSafe<std::optional<uint32_t*>, nToRetOn, &bConv, &insnMatch<>, retCount, szBytes>(addr);
        }
    }
}
