#pragma once
#include <array>
#include <optional>
#include <tuple>
#include "capstone/shared/capstone/capstone.h"
#include "capstone/shared/platform.h"
#include "log.h"

namespace cs {
csh getHandle();

uint32_t* readb(uint32_t const* addr);

template <arm64_insn... args>
constexpr bool insnMatch(cs_insn* insn) {
  if constexpr (sizeof...(args) > 0) {
    return (((insn->id == args) || ...));
  }
  return false;
};

struct AddrSearchPair {
  AddrSearchPair(uint32_t const* addr_, uint32_t remSearchSize_) : addr(addr_), remSearchSize(remSearchSize_) {}
  uint32_t const* addr;
  uint64_t remSearchSize;
};

auto find_through_hooks(void const* hook, uint32_t initialSearchSize, auto&& func) {
  return func(cs::AddrSearchPair(reinterpret_cast<uint32_t const*>(hook), initialSearchSize));
}

template <std::size_t sz, class F1, class F2>
decltype(auto) findNth(std::array<AddrSearchPair, sz>& addrs, uint32_t nToRetOn, int retCount, F1&& match, F2&& skip) {
  cs_insn* insn = cs_malloc(getHandle());
  for (std::size_t searchIdx = 0; searchIdx < addrs.size(); searchIdx++) {
    while (addrs[searchIdx].remSearchSize > 0) {
      auto ptr = reinterpret_cast<uint64_t>(addrs[searchIdx].addr);
      bool res = cs_disasm_iter(getHandle(), reinterpret_cast<uint8_t const**>(&addrs[searchIdx].addr),
                                &addrs[searchIdx].remSearchSize, &ptr, insn);
      LOG_DEBUG("{} diassemb: {} (rCount: {}, nToRetOn: {}, sz: {})", fmt::ptr((void*)ptr), insn->mnemonic, retCount,
                nToRetOn, addrs[searchIdx].remSearchSize);
      if (res) {
        // Valid decode, so lets check to see if it is a match or we need to break.
        if (insn->id == ARM64_INS_RET) {
          if (retCount == 0) {
            // Early termination!
            cs_free(insn, 1);
            LOG_WARN("Could not find: {} call at: {} within: {} rets! Found all of the rets first!", nToRetOn,
                     fmt::ptr(addrs[searchIdx].addr), retCount);
            return (decltype(match(insn)))std::nullopt;
          }
          retCount--;
        } else {
          auto testRes = match(insn);
          if (testRes) {
            if (nToRetOn == 1) {
              cs_free(insn, 1);
              return testRes;
            } else {
              nToRetOn--;
            }
          } else if (skip(insn)) {
            if (nToRetOn == 1) {
              std::string name(insn->mnemonic);
              cs_free(insn, 1);
              LOG_WARN(
                  "Found: {} match, at: {} within: {} rets, but the result was a {}! Cannot compute destination "
                  "address!",
                  nToRetOn, fmt::ptr(addrs[searchIdx].addr), retCount, name);
              return (decltype(match(insn)))std::nullopt;
            } else {
              nToRetOn--;
            }
          }
        }
        // Other instructions are ignored silently
      } else {
        // Invalid instructions are ignored silently.
        // In order to skip these properly, we must increment our instructions, ptr, and size accordingly.
        addrs[searchIdx].remSearchSize -= 4;
        addrs[searchIdx].addr++;
      }
    }
    // We didn't find it. Let's instead look at the next address/size pair for a match.
    LOG_DEBUG("Could not find: {} call at: {} within: {} rets at idx: {}!", nToRetOn, fmt::ptr(addrs[searchIdx].addr),
              retCount, searchIdx);
  }
  // If we run out of bytes to parse, we fail
  cs_free(insn, 1);
  return (decltype(match(insn)))std::nullopt;
}

template <uint32_t nToRetOn, int retCount = -1, size_t szBytes = 4096, class F1, class F2>
  requires((nToRetOn >= 1 && (szBytes % 4) == 0))
auto findNth(uint32_t const* addr, F1&& match, F2&& skip) {
  cs_insn* insn = cs_malloc(getHandle());
  auto ptr = reinterpret_cast<uint64_t>(addr);
  auto instructions = reinterpret_cast<uint8_t const*>(addr);

  int rCount = retCount;
  uint32_t nCalls = nToRetOn;
  size_t sz = szBytes;
  while (sz > 0) {
    bool res = cs_disasm_iter(getHandle(), &instructions, &sz, &ptr, insn);
    LOG_DEBUG("{} diassemb: {} (rCount: {}, nCalls: {}, sz: {})", fmt::ptr(ptr), insn->mnemonic, rCount, nCalls, sz);
    if (res) {
      // Valid decode, so lets check to see if it is a match or we need to break.
      if (insn->id == ARM64_INS_RET) {
        if (rCount == 0) {
          // Early termination!
          cs_free(insn, 1);
          LOG_WARN("Could not find: {} call at: {} within: {} rets! Found all of the rets first!", nToRetOn,
                   fmt::ptr(ptr), retCount);
          return (decltype(match(insn)))std::nullopt;
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
            LOG_WARN(
                "Found: {} match, at: {} within: {} rets, but the result was a {}! Cannot compute destination address!",
                nToRetOn, fmt::ptr(ptr), retCount, name);
            return (decltype(match(insn)))std::nullopt;
          } else {
            nCalls--;
          }
        }
      }
      // Other instructions are ignored silently
    } else {
      // Invalid instructions are ignored silently.
      // In order to skip these properly, we must increment our instructions, ptr, and size accordingly.
      sz -= 4;
      ptr += 4;
      instructions += 4;
    }
  }
  // If we run out of bytes to parse, we fail
  cs_free(insn, 1);
  LOG_WARN("Could not find: {} call at: {} within: {} rets, within size: {}!", nToRetOn, fmt::ptr(addr), retCount,
           szBytes);
  return (decltype(match(insn)))std::nullopt;
}

template <uint32_t nToRetOn, auto match, auto skip, int retCount = -1, size_t szBytes = 4096>
  requires((nToRetOn >= 1 && (szBytes % 4) == 0))
auto findNth(uint32_t const* addr) {
  cs_insn* insn = cs_malloc(getHandle());
  auto ptr = reinterpret_cast<uint64_t>(addr);
  auto instructions = reinterpret_cast<uint8_t const*>(addr);

  int rCount = retCount;
  uint32_t nCalls = nToRetOn;
  size_t sz = szBytes;
  while (sz > 0) {
    bool res = cs_disasm_iter(getHandle(), &instructions, &sz, &ptr, insn);
    LOG_DEBUG("{} diassemb: {} (rCount: {}, nCalls: {}, sz: {})", fmt::ptr((void*)ptr), insn->mnemonic, rCount, nCalls,
              sz);
    if (res) {
      // Valid decode, so lets check to see if it is a match or we need to break.
      if (insn->id == ARM64_INS_RET) {
        if (rCount == 0) {
          // Early termination!
          cs_free(insn, 1);
          LOG_WARN("Could not find: {} call at: {} within: {} rets! Found all of the rets first!", nToRetOn,
                   fmt::ptr((void*)ptr), retCount);
          return (decltype(match(insn)))std::nullopt;
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
            LOG_WARN(
                "Found: {} match, at: {} within: {} rets, but the result was a {}! Cannot compute destination address!",
                nToRetOn, fmt::ptr((void*)ptr), retCount, name);
            return (decltype(match(insn)))std::nullopt;
          } else {
            nCalls--;
          }
        }
      }
      // Other instructions are ignored silently
    } else {
      // Invalid instructions are ignored silently.
      // In order to skip these properly, we must increment our instructions, ptr, and size accordingly.
      LOG_WARN("FAILED PARSE: {} diassemb: 0x{:x}", fmt::ptr((void*)ptr), *(uint32_t*)ptr);
      sz -= 4;
      ptr += 4;
      instructions += 4;
    }
  }
  // If we run out of bytes to parse, we fail
  cs_free(insn, 1);
  return (decltype(match(insn)))std::nullopt;
}

std::optional<uint32_t*> blConv(cs_insn* insn);

template <uint32_t nToRetOn, bool includeR = false, int retCount = -1, size_t szBytes = 4096>
  requires((nToRetOn >= 1 && (szBytes % 4) == 0))
auto findNthBl(uint32_t const* addr) {
  return find_through_hooks(addr, szBytes, [](auto... pairs) {
    std::array addrs{ pairs... };
    if constexpr (includeR) {
      return findNth(addrs, nToRetOn, retCount, &blConv, &insnMatch<ARM64_INS_BLR>);
    } else {
      return findNth(addrs, nToRetOn, retCount, &blConv, &insnMatch<>);
    }
  });
}

std::optional<uint32_t*> bConv(cs_insn* insn);

template <uint32_t nToRetOn, bool includeR = false, int retCount = -1, size_t szBytes = 4096>
  requires((nToRetOn >= 1 && (szBytes % 4) == 0))
auto findNthB(uint32_t const* addr) {
  return find_through_hooks(addr, szBytes, [](auto... pairs) {
    std::array addrs{ pairs... };
    if constexpr (includeR) {
      return findNth(addrs, nToRetOn, retCount, &bConv, &insnMatch<ARM64_INS_BR>);
    } else {
      return findNth(addrs, nToRetOn, retCount, &bConv, &insnMatch<>);
    }
  });
}

std::optional<std::tuple<uint32_t*, arm64_reg, uint32_t*>> pcRelConv(cs_insn* insn);

template <uint32_t nToRetOn, int retCount = -1, size_t szBytes = 4096>
  requires((nToRetOn >= 1 && (szBytes % 4) == 0))
auto findNthPcRel(uint32_t const* addr) {
  return find_through_hooks(addr, szBytes, [](auto... pairs) {
    std::array addrs{ pairs... };
    return findNth(addrs, nToRetOn, retCount, &pcRelConv, &insnMatch<>);
  });
}

std::optional<std::tuple<uint32_t*, arm64_reg, int64_t>> regMatchConv(cs_insn* match, arm64_reg toMatch);

template <uint32_t nToRetOn, int retCount = -1, size_t szBytes = 4096>
  requires((nToRetOn >= 1 && (szBytes % 4) == 0))
auto findNthReg(uint32_t const* addr, arm64_reg reg) {
  auto lmd = [reg](cs_insn* in) -> std::optional<std::tuple<uint32_t*, arm64_reg, int64_t>> {
    return regMatchConv(in, reg);
  };
  return find_through_hooks(addr, szBytes, [lmd = std::move(lmd)](auto... pairs) {
    std::array addrs{ pairs... };
    return findNth(addrs, nToRetOn, retCount, lmd, &insnMatch<>);
  });
}

template <uint32_t nToRetOn, uint32_t nImmOff, size_t szBytes = 4096>
  requires((nToRetOn >= 1 && nImmOff >= 1 && (szBytes % 4) == 0))
std::optional<std::tuple<uint32_t*, arm64_reg, uint32_t*>> getpcaddr(uint32_t const* addr) {
  auto pcrel = findNthPcRel<nToRetOn, -1, szBytes>(addr);
  // SAFE_ABORT_MSG("Could not find: %u pcrel at: %p within: %i rets, within size: %zu!", nToRetOn, addr, -1, szBytes);
  if (!pcrel) return std::nullopt;
  // addr is in first slot of tuple, reg in second, dst imm in third
  // TODO: decrease size correctly
  auto reginst = findNthReg<nImmOff, -1, szBytes>(std::get<0>(*pcrel), std::get<1>(*pcrel));
  // SAFE_ABORT_MSG("Could not find: %u reg with reg: %u at: %p within: %i rets, within size: %zu!", nImmOff,
  // std::get<1>(*pcrel), std::get<0>(*pcrel), -1, szBytes);
  if (!reginst) return std::nullopt;
  return std::make_tuple(
      std::get<0>(*reginst), std::get<1>(*reginst),
      reinterpret_cast<uint32_t*>(reinterpret_cast<uint64_t>(std::get<2>(*pcrel)) + std::get<2>(*reginst)));
}

template <uint32_t nToRetOn, uint32_t nImmOff, int match, size_t szBytes = 4096>
  requires((nToRetOn >= 1 && nImmOff >= 1 && (szBytes % 4) == 0))
std::optional<uint32_t*> evalswitch(uint32_t const* addr) {
  // Get matching adr/adrp + offset on register
  auto res = getpcaddr<nToRetOn, nImmOff, szBytes>(addr);
  // SAFE_ABORT_MSG("Could not find: %u pcrel at: %p within: %i rets, within size: %zu!", nToRetOn, addr, -1, szBytes);
  if (!res) return std::nullopt;
  // Convert destination to the switch table address
  auto switchTable = reinterpret_cast<int32_t*>(std::get<2>(*res));
  // Index into switch table, which holds int32s, offset from start of switch table
  auto val = switchTable[match - 1];
  // Add offset to switch table and convert back to pointer type
  return reinterpret_cast<uint32_t*>(reinterpret_cast<uint64_t>(switchTable) + val);
}

template <auto ins>
static std::optional<uint32_t*> findIns(cs_insn* insn) {
  return (insn->id == ins) ? std::optional<uint32_t*>(reinterpret_cast<uint32_t*>(insn->address)) : std::nullopt;
}

struct TBZ {
  uint32_t reg : 5;      // which register to use, 0-31
  uint32_t offset : 14;  // offset is in instructions, not in bytes
  uint32_t test : 5;     // which bits to test, also relates to the width variable
  uint32_t opc : 7;      // opcode
  uint32_t width : 1;    // whether this is an x or w register, 0 meaning W and 1 meaning X
};
static_assert(sizeof(TBZ) == sizeof(uint32_t));

/// @return optional tuple containing found tbz ins, offset it requests off of itself, and the offset it points to, or
/// nullopt if not found
template <auto ret_on>
static std::optional<std::tuple<uint32_t*, uint32_t, uint32_t*>> getTbzAddr(uint32_t* address) {
  // the label to jump to for tbz is 14 bits wide
  auto tbz = cs::findNth<ret_on, &findIns<ARM64_INS_TBZ>, cs::insnMatch<>, 1>(address);
  if (!tbz) return std::nullopt;

  auto t = reinterpret_cast<TBZ*>(*tbz);
  return { { *tbz, (uint32_t)t->offset, *tbz + t->offset } };
}

struct BCond {
  uint32_t cond : 4;        // arm64 conditional
  const uint32_t zero : 1;  // always 0
  uint32_t offset : 19;     // offset off of this instruction
  uint32_t opc : 8;         // opcode
};
static_assert(sizeof(BCond) == sizeof(uint32_t));

/// @brief matches a b.cond where cond is an arm64_cc conditional
template <arm64_cc condition>
static std::optional<uint32_t*> matchBCond(cs_insn* insn) {
  static constexpr auto b_cond_ins = 0x54;
  // the arm64_cc enum is consistently the actual conditionals + 1
  static constexpr auto actual_condition = (condition - 1);
  auto const bcond = reinterpret_cast<BCond*>(insn->address);
  // if we are not a b conditional
  if (bcond->opc != b_cond_ins) return std::nullopt;
  if (bcond->cond != actual_condition) return std::nullopt;
  return reinterpret_cast<uint32_t*>(insn->address);
}

/// @brief gets the nth bCondAddr, and returns a tuple containing the address of the found b.cond, the offset it will
/// jump and the target address. returns nullopt if not found
template <size_t ret_on, arm64_cc condition>
static std::optional<std::tuple<uint32_t*, uint32_t, uint32_t*>> getBCondAddr(uint32_t* address) {
  auto bcond = cs::findNth<ret_on, &matchBCond<condition>, &cs::insnMatch<>, 1>(address);
  if (!bcond) return std::nullopt;

  auto bc = reinterpret_cast<BCond*>(*bcond);
  return { { *bcond, (uint32_t)bc->offset, *bcond + bc->offset } };
}

struct Movz {
  uint32_t reg : 5;     // which register to store the value in
  uint32_t value : 16;  // value
  uint32_t hw : 2;      // optional left shift of value
  uint32_t opc : 8;     // opcode
  uint32_t sf : 1;      // 32 or 64 bit register, 0 for 32, 1 for 64
};
static_assert(sizeof(Movz) == sizeof(uint32_t));

/// @brief gets the nth movz instruction, and returns a tuple containing it's address and the value it moves
template <size_t ret_on>
static std::optional<std::tuple<uint32_t*, uint64_t>> getMovzValue(uint32_t* address) {
  auto movz = cs::findNth<ret_on, &findIns<ARM64_INS_MOVZ>, &cs::insnMatch<>, 1>(address);
  if (!movz) return std::nullopt;

  auto m = reinterpret_cast<Movz*>(*movz);

  auto shift = (m->sf ? m->hw : m->hw & 0b1) * 16;
  return { { *movz, ((uint64_t)m->value << shift) } };
}
}  // namespace cs
