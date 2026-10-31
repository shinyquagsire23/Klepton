#include "fixups.hpp"
#include <fmt/core.h>
#include <sys/mman.h>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <list>
#include <span>
#include "capstone/capstone.h"
#include "capstone/platform.h"
#if !defined(GIT_COMMIT) && __has_include("git_info.inc")
#include "git_info.inc"
#endif
#include "util.hpp"

namespace {
auto get_branch_immediate(cs_insn const& inst) {
  FLAMINGO_ASSERT(inst.detail->arm64.op_count == 1);
  return inst.detail->arm64.operands[0].imm;
}

std::pair<uint8_t, int64_t> get_second_immediate(cs_insn const& inst) {
  // register is just bottom 5 bits
  constexpr uint32_t reg_mask = 0b11111;
  FLAMINGO_ASSERT(inst.detail->arm64.op_count == 2);
  return { *reinterpret_cast<uint32_t const*>(inst.bytes) & reg_mask, inst.detail->arm64.operands[1].imm };
}

std::pair<uint8_t, int64_t> get_last_immediate(cs_insn const& inst) {
  // register is just bottom 5 bits
  constexpr uint32_t reg_mask = 0b11111;
  FLAMINGO_ASSERT(inst.detail->arm64.op_count >= 2);
  return { *reinterpret_cast<uint32_t const*>(inst.bytes) & reg_mask,
           inst.detail->arm64.operands[inst.detail->arm64.op_count - 1].imm };
}

constexpr int64_t get_untagged_pc(uint64_t pc) {
  // Upper byte is tagged for PC addresses on android 11+
  constexpr uint64_t mask = ~(0xFFULL << (64U - 8U));
  return static_cast<int64_t>(static_cast<uint64_t>(pc) & mask);
}
int64_t get_untagged_pc(void const* pc) {
  return get_untagged_pc(reinterpret_cast<uint64_t>(pc));
}

// Helper types for holding immediate masks, lshifts and rshifts for conversions to immediates from PC differences
template <arm64_insn>
struct BranchImmTypeTrait;

template <arm64_insn type>
  requires(type == ARM64_INS_B || type == ARM64_INS_BL)
struct BranchImmTypeTrait<type> {
  constexpr static uint32_t imm_mask = 0b00000011111111111111111111111111U;
  constexpr static uint32_t lshift = 0;
  constexpr static uint32_t rshift = 2;
};

template <arm64_insn type>
  requires(type == ARM64_INS_CBZ || type == ARM64_INS_CBNZ)
struct BranchImmTypeTrait<type> {
  constexpr static uint32_t imm_mask = 0b00000000111111111111111111100000;
  constexpr static uint32_t lshift = 5;
  constexpr static uint32_t rshift = 2;
};

template <arm64_insn type>
  requires(type == ARM64_INS_TBZ || type == ARM64_INS_TBNZ)
struct BranchImmTypeTrait<type> {
  constexpr static uint32_t imm_mask = 0b00000000000001111111111111100000;
  constexpr static uint32_t lshift = 5;
  constexpr static uint32_t rshift = 2;
};

/// @brief Helper function that returns an encoded b for a particular offset
consteval uint32_t get_b(int offset) {
  constexpr uint32_t b_opcode = 0b00010100000000000000000000000000U;
  return (b_opcode | (static_cast<uint32_t>(offset) >> 2U));
}

constexpr uint32_t ldr_imm_mask = 0b111111111111111111100000U;

struct ImmediateReferenceTag {
  /// @brief The immediate mask to use when rewriting the instruction
  uint32_t imm_mask;
  /// @brief The amount to shift the immediate to the left to encode it correctly such that the mask would be valid
  uint_fast16_t lshift;
  /// @brief The amount to shift the immediate to the right to encode it correctly such that the mask would be valid
  uint_fast16_t rshift;
  /// @brief Index of the fixup index with the immediate to be overwritten
  uint_fast16_t fixup_index;
  /// @brief Index into the resultant data section to resolve to
  uint_fast16_t data_index;
};
struct BranchReferenceTag {
  /// @brief The immediate mask to use when rewriting the instruction
  uint32_t imm_mask;
  /// @brief The amount to shift the immediate to the left to encode it correctly such that the mask would be valid
  uint32_t lshift;
  /// @brief The amount to shift the immediate to the right to encode it correctly such that the mask would be valid
  uint32_t rshift;
  /// @brief Index to overwrite
  uint32_t target_index;
};
struct DataEntry {
  /// @brief The data to hold in this entry
  uint32_t data;
  /// @brief The alignment (in multiples of 4 bytes) that we should perform for this entry
  uint_fast8_t alignment;
  /// @brief The actual index into the data section this data entry maps to.
  /// This is only assigned to AFTER all data entries have been added.
  uint32_t actual_idx{};
};
// Holds the context for performing fixups that we don't want to expose to the caller.
struct FixupContext {
  // The initial target pointer
  std::span<uint32_t const> target;
  flamingo::ProtectionWriter<uint32_t> fixup_writer;
  // Holds sequentially laid out data for usage within fixups
  std::vector<DataEntry> data_block{};
  uint_fast16_t data_index = 0;
  // Holds a collection of data elements, which describes which fixups to perform overwrites of after data is allocated
  std::vector<ImmediateReferenceTag> data_ref_tags{};
  // Holds a mapping from fixup index to a list of branches targeting that location
  std::vector<std::list<BranchReferenceTag>> branch_ref_map{};
  // Holds the mapping of target index to fixup index for branch references
  // TODO: Technically, we need to see if ANY branch target would leave us in ANY fixup block...
  // TODO: Should collect a set of references so that if we ever install a hook over somewhere we would jump to we would
  // force a recompile. We should check against the full set of all fixups for this.
  std::vector<uint32_t> target_to_fixups{};
  // The raw address of the target start/end as an untagged PC address
  uint64_t target_start;
  uint64_t target_end;

  FixupContext(flamingo::PointerWrapper<uint32_t> fixup_ptr, std::span<uint32_t const> target)
      : target(target),
        fixup_writer(fixup_ptr),
        target_start(get_untagged_pc(target.data())),
        target_end(get_untagged_pc(&target[target.size()])) {
    target_to_fixups.resize(target.size());
    branch_ref_map.resize(target.size());
    // based off of the size of the fixups, we allocate accordingly.
    // TODO: This is an overestimate (each fixup needs a uint64_t)
    data_block.reserve(target.size() * 2);
    data_ref_tags.reserve(target.size());
  }

  auto GetFixupPC() const {
    return get_untagged_pc(&fixup_writer.target.addr[fixup_writer.target_offset]);
  }

  auto Write(uint32_t inst) {
    return fixup_writer.Write(inst);
  }
  void WriteData(uint_fast16_t fixup_idx, uint32_t data, uint32_t imm_mask, uint_fast16_t lshift,
                 uint_fast16_t rshift) {
    FLAMINGO_ASSERT(fixup_idx < fixup_writer.target_offset);
    uint_fast16_t data_index = data_block.size();
    // Pointer is known to be little endian
    FLAMINGO_DEBUG("Adding 32b data: 0x{:x} at data index: {} for fixup index: {} ({})", data, data_index, fixup_idx,
                   fmt::ptr(&fixup_writer.target.addr[fixup_idx]));
    data_block.push_back({ .data = data, .alignment = 1 });
    data_ref_tags.emplace_back(ImmediateReferenceTag{
      .imm_mask = imm_mask,
      .lshift = lshift,
      .rshift = rshift,
      .fixup_index = fixup_idx,
      .data_index = data_index,
    });
  }
  // When we call WriteData, we are using the previously written fixup as our fixup index. This means, however, that we
  // must have written at least one fixup already.
  void WriteData(uint_fast16_t fixup_idx, uint64_t large_data, uint32_t imm_mask, uint_fast16_t lshift,
                 uint_fast16_t rshift) {
    FLAMINGO_ASSERT(fixup_idx < fixup_writer.target_offset);
    uint_fast16_t data_index = data_block.size();
    // Pointer is known to be little endian
    FLAMINGO_DEBUG("Adding 64b data: 0x{:x} at data index: {} for fixup index: {} ({})", large_data, data_index,
                   fixup_idx, fmt::ptr(&fixup_writer.target.addr[fixup_idx]));
    // The first entry is aligned 64, the second entry has 32b alignment.
    data_block.push_back({ .data = static_cast<uint32_t>(large_data & (UINT32_MAX)), .alignment = 2 });
    data_block.push_back({ .data = static_cast<uint32_t>((large_data >> 32) & UINT32_MAX), .alignment = 1 });
    data_ref_tags.emplace_back(ImmediateReferenceTag{
      .imm_mask = imm_mask,
      .lshift = lshift,
      .rshift = rshift,
      .fixup_index = fixup_idx,
      .data_index = data_index,
    });
  }
  void WriteLdrWithData(int64_t data, uint_fast8_t reg) {
    // https://developer.arm.com/documentation/ddi0596/2021-12/Base-Instructions/LDR--literal---Load-Register--literal--
    // imm is encoded as << 2, LSB just to the right of reg
    constexpr uint32_t ldr_mask = 0b01011000000000000000000000000000U;
    constexpr uint32_t reg_mask = 0b11111;
    auto ldr_idx = Write(ldr_mask | (reg_mask & reg));
    WriteData(ldr_idx, static_cast<uint64_t>(data), ldr_imm_mask, 5, 2);
  }
  void WriteLdrBrData(int64_t target) {
    // LDR x17, DATA OFFSET FOR BRANCH TARGET
    WriteLdrWithData(target, 17);
    // BR x17
    constexpr uint32_t br_x17 = 0xD61F0220U;
    Write(br_x17);
  }
  void WriteCallback(uint32_t const* target) {
    constexpr uint32_t branch_imm_mask = 0b00000011111111111111111111111111U;
    auto delta = get_untagged_pc(target) - GetFixupPC();
    if (std::llabs(delta) > (branch_imm_mask << 1) + 1) {
      // Too far for b. Emit a br instead.
      WriteLdrBrData(get_untagged_pc(target));
    } else {
      // Small enough to emit a b.
      // b opcode | encoded immediate (delta >> 2)
      // Note, abs(delta >> 2) must be < (1 << 26)
      constexpr uint32_t b_opcode = 0b00010100000000000000000000000000U;
      Write(b_opcode | ((delta >> 2) & branch_imm_mask));
    }
  }
  void WriteB(int64_t imm) {
    // https://developer.arm.com/documentation/ddi0596/2021-12/Base-Instructions/B--Branch-
    WriteCallback(reinterpret_cast<uint32_t*>(imm));
  }

  void WriteBl(int64_t imm) {
    // https://developer.arm.com/documentation/ddi0596/2021-12/Base-Instructions/BL--Branch-with-Link-
    constexpr uint32_t branch_imm_mask = 0b00000011111111111111111111111111U;
    int64_t delta = imm - GetFixupPC();
    if (std::llabs(delta) > (branch_imm_mask << 1) + 1) {
      // Too far to emit a b. Emit a br instead.
      // We CAN emit a blr here because the pc + 4 for return will no longer be in the data section.
      // LDR X17, DATA OFFSET FOR BRANCH
      WriteLdrWithData(imm, 17);
      // BLR x17
      constexpr uint32_t blr_x17 = 0xD63F0220U;
      Write(blr_x17);
    } else {
      // Small enough to emit a b/bl.
      // bl opcode | encoded immediate (delta >> 2)
      // Note, abs(delta >> 2) must be < (1 << 26)
      constexpr uint32_t bl_opcode = 0b10010100000000000000000000000000U;
      Write(bl_opcode | ((delta >> 2) & branch_imm_mask));
    }
  }

  void WriteAdr(uint8_t reg, int64_t imm) {
    // https://developer.arm.com/documentation/ddi0596/2021-12/Base-Instructions/ADR--Form-PC-relative-address-?lang=en
    constexpr uint32_t adr_maximum_imm = 0b00000000000111111111111111111111U;
    constexpr uint32_t reg_mask = 0b11111;
    int64_t delta = imm - GetFixupPC();
    if (std::llabs(delta) >= (adr_maximum_imm >> 1U)) {
      // Too far to emit just an adr.
      // LDR (used register), DATA OFSET FOR IMM
      WriteLdrWithData(imm, reg);
    } else {
      // Close enough to emit an adr.
      // Note that delta should be within +-1 MB
      constexpr uint32_t adr_opcode = 0b00010000000000000000000000000000;
      // Get immlo
      uint32_t imm_lo = ((static_cast<uint32_t>(delta) & 3U) << 29);
      // Get immhi
      uint32_t imm_hi = (static_cast<uint32_t>(delta) >> 2U) << 5;
      Write(adr_opcode | imm_lo | imm_hi | (reg_mask & reg));
    }
  }

  void WriteAdrp(uint8_t reg, int64_t imm) {
    // https://developer.arm.com/documentation/ddi0596/2021-12/Base-Instructions/ADR--Form-PC-relative-address-?lang=en
    // constexpr uint32_t adr_maximum_imm = 0b00000000000111111111111111111111U;
    constexpr uint32_t pc_imm_mask = ~0b111111111111U;
    constexpr int64_t adrp_maximum_imm = 0xFFFFF000U;
    int64_t delta = (GetFixupPC() & pc_imm_mask) - imm;

    if (std::llabs(delta) < adrp_maximum_imm) {
      // TODO: Note missed optimization opportunity
      // TODO: Should perform a small ADRP
      FLAMINGO_DEBUG("Potentially missed optimization opportunity for near ADRP, imm: {}, target pc: {}", imm,
                     GetFixupPC());
      // This optimization fails sometimes:
      // Orig: D flamingo|v0.1.0: Addr: 0x7b6d100 Value: 0xb000eee0, adrp x0, #0x994a000
      // Fixup: D flamingo|v0.1.0: Addr: 0x7fb68fd0903c Value: 0xf0431de0, adrp x0, #0x7fb7160c8000

      // // Close enough to emit an adrp.
      // // Note that delta should be within +-4 GB
      // constexpr uint32_t adrp_opcode = 0b10010000000000000000000000000000U;
      // constexpr uint32_t reg_mask = 0b11111;
      // // Imm is << 12 in parse of instruction
      // delta >>= 12;
      // // Get immlo
      // uint32_t imm_lo = ((static_cast<uint32_t>(delta) & 3) << 29);
      // // Get immhi
      // uint32_t imm_hi = (static_cast<uint32_t>(delta) >> 2) << 5;
      // Write(adrp_opcode | imm_lo | imm_hi | (reg_mask & reg));
    }
    // Too far to emit just an adr.
    // LDR (used register), DATA OFFSET FOR IMM
    WriteLdrWithData(imm, reg);
  }

  void WriteLdr(uint32_t inst, uint8_t reg, int64_t imm) {
    // https://developer.arm.com/documentation/ddi0596/2021-12/Base-Instructions/LDR--literal---Load-Register--literal--
    // 20 bits because signed range is only allowed
    constexpr int64_t max_ldr_range = (1LL << 20);
    if ((inst & 0xFF000000U) == 0xD8000000U) {
      // This is a prefetch instruction.
      // Lets just skip it.
      return;
    }
    int64_t delta = imm - GetFixupPC();
    if (std::llabs(delta) < max_ldr_range) {
      // TODO: Should perform a small LDR
      // constexpr uint32_t reg_mask = 0b11111;
      FLAMINGO_DEBUG("Potentially missed optimization opportunity for near LDR, imm: {} target pc: {}", imm,
                     GetFixupPC());
    }

    // Too far to emit an equivalent LDR
    // Fallback to performing a direct memory write/read
    // LDR (used register), DATA OFFSET FOR IMM
    // TODO: Instead of assuming int64_t data at imm, use the size_mask and swap accordingly
    // 4, 8, 16(?) for our sizes
    // constexpr uint32_t size_mask = 0x40000000U;
    WriteLdrWithData(*reinterpret_cast<int64_t*>(imm), reg);
  }
  template <bool imm_19>
  void WriteCondBranch(uint32_t instruction, int64_t imm) {
    uint32_t imm_mask;
    if constexpr (imm_19) {
      // Imm 19
      constexpr uint32_t imm_mask_19 = 0b00000000111111111111111111100000;
      imm_mask = imm_mask_19;
    } else {
      // Imm 14
      constexpr uint32_t imm_mask_14 = 0b00000000000001111111111111100000;
      imm_mask = imm_mask_14;
    }
    int64_t delta = imm - GetFixupPC();
    // imm_mask >> 1 for maximum positive value
    // << 2 because branch imms are << 2
    // >> 5 because the mask is too high
    if (std::llabs(delta) < (imm_mask >> 4)) {
      // Small enough to optimize, just write the instruction
      // But with the modified offset
      // Delta should be >> 2 for branch imm
      // Then << 5 to be in the correct location
      Write((instruction & ~imm_mask) | (static_cast<uint32_t>((delta >> 2) << 5) & imm_mask));
    } else {
      // Otherwise, we need to write the same expression but with a known offset
      // Specifically, write the instruction but with an offset of 8 (to skip the B we write later in this function)
      // 2, because 8 >> 2 is 2
      // << 5 to place in correct location for immediate
      Write((instruction & ~imm_mask) | ((2 << 5) & imm_mask));
      // 0xC to skip over: LDR x17, BR x17
      // TODO: Instead of hardcoding the b and forcing an ldr + br data entry, for near immediates, we can ecode them
      // closer

      Write(get_b(0xC));
      WriteLdrBrData(imm);
    }
  }

  template <arm64_insn type>
  bool TryDeferBranch(uint16_t i, int64_t dst, uint32_t inst) {
    // If it is a branch, check to see if the target immediate would place us within our fixup range
    // If so, we need to:
    // - If the target is behind us, use the new target directly
    // - If the target is in front of us, defer the write until later.
    // We defer the write by basically writing the branch itself (since it must be a close branch)
    // and then add its index (and its immediate mask and shift amount) to some set.
    // Then, when we start the fixup for an instruction, we check the set to see if we should go back and fix the
    // specified indices. To fix them, we simply walk all of the indices we wish to fix, and for each:
    // - Current PC of instruction - &target[index] to replace
    // - Use as argument for shift + mask?

    // If we are within OUR fixup range, that's when things get interesting.
    // TODO: If we are in SOME OTHER TRAMPOLINE'S fixup range, then we should use their call
    using trait_t = BranchImmTypeTrait<type>;
    constexpr uint32_t imm_mask = trait_t::imm_mask;
    constexpr uint32_t lshift = trait_t::lshift;
    constexpr uint32_t rshift = trait_t::rshift;
    if (dst < static_cast<int64_t>(target_end) && dst >= static_cast<int64_t>(target_start)) {
      FLAMINGO_DEBUG("Potentially deferring branch at: 0x{:x} because it is within: 0x{:x} and 0x{:x}", dst,
                     target_start, target_end);
      auto target_offset = (dst - target_start) / sizeof(uint32_t);
      FLAMINGO_ASSERT(target_offset < target_to_fixups.size());
      FLAMINGO_ASSERT(target_offset < branch_ref_map.size());
      // Always emit the instruction with AN immediate that is valid.
      // For forward references, we need to defer.
      // This difference could be negative, but for those cases we will defer and overwrite.
      auto fixup_difference = static_cast<uint32_t>(
          get_untagged_pc(reinterpret_cast<uint64_t>(&fixup_writer.target.addr[fixup_writer.target_offset])) -
          get_untagged_pc(reinterpret_cast<uint64_t>(&fixup_writer.target.addr[target_to_fixups[target_offset]])));
      Write((inst & ~imm_mask) | ((fixup_difference >> rshift) << lshift));
      if (target_offset > i) {
        FLAMINGO_DEBUG("Deferring at: {} with target offset: 0x{:x}", i, target_offset);
        // Need to defer.
        // Deference SHOULD never cause the instruction being deferred to expand in size.
        // It should always be possible to point the deferred instruction to the new one without emitting more
        // instructions
        branch_ref_map[target_offset].emplace_back(BranchReferenceTag{
          .imm_mask = imm_mask,
          .lshift = lshift,
          .rshift = rshift,
          .target_index = i,
        });
      }
      return true;
    }
    return false;
  }
  void PerformFixupFor(cs_insn const& inst, int i, uint32_t const* const current_inst_ptr) {
    // Set the target map entry for this incoming instruction to the current offset of the output
    target_to_fixups[i] = fixup_writer.target_offset;
    switch (inst.id) {
      // Handle fixups for branch immediate
      case ARM64_INS_B: {
        FLAMINGO_DEBUG("Fixing up B...");
        auto dst = get_branch_immediate(inst);
        if (!TryDeferBranch<ARM64_INS_B>(i, dst, *current_inst_ptr)) {
          if (inst.detail->arm64.cc != ARM64_CC_INVALID) {
            WriteCondBranch<true>(*current_inst_ptr, dst);
          } else {
            WriteB(dst);
          }
        }
      } break;
      case ARM64_INS_BL: {
        FLAMINGO_DEBUG("Fixing up BL...");
        auto dst = get_branch_immediate(inst);
        if (!TryDeferBranch<ARM64_INS_BL>(i, dst, *current_inst_ptr)) {
          WriteBl(dst);
        }
      } break;

      // Handle fixups for conditional branches
      case ARM64_INS_CBNZ:
      case ARM64_INS_CBZ: {
        FLAMINGO_DEBUG("Fixing up CBNZ/CBZ...");
        auto [reg, dst] = get_last_immediate(inst);
        if (!TryDeferBranch<ARM64_INS_CBNZ>(i, dst, *current_inst_ptr)) {
          WriteCondBranch<true>(*current_inst_ptr, dst);
        }
      } break;
      case ARM64_INS_TBNZ:
      case ARM64_INS_TBZ: {
        FLAMINGO_DEBUG("Fixing up TBNZ/TBZ...");
        auto [reg, dst] = get_last_immediate(inst);
        if (!TryDeferBranch<ARM64_INS_TBNZ>(i, dst, *current_inst_ptr)) {
          WriteCondBranch<false>(*current_inst_ptr, dst);
        }
      } break;

      // Handle fixups for load literals
      case ARM64_INS_LDR: {
        FLAMINGO_DEBUG("Fixing up LDR...");
        // TODO: Handle the case where an LDR would land in a fixup range, so we need to copy the raw values
        constexpr uint32_t b_31 = 0b10000000000000000000000000000000;
        constexpr uint32_t ldr_lit_opc_mask = 0b10111111000000000000000000000000;
        if ((*current_inst_ptr & ldr_lit_opc_mask) == 0b00011000000000000000000000000000) {
          // This is an ldr literal
          // https://developer.arm.com/documentation/ddi0596/2021-12/Base-Instructions/LDR--literal---Load-Register--literal--
          auto [reg, dst] = get_second_immediate(inst);
          WriteLdr(*current_inst_ptr, reg, dst);
        } else if ((*current_inst_ptr & (ldr_lit_opc_mask & ~b_31)) == 0b00011100000000000000000000000000) {
          // This is an ldr literal, SIMD
          // https://developer.arm.com/documentation/ddi0596/2021-12/SIMD-FP-Instructions/LDR--literal--SIMD-FP---Load-SIMD-FP-Register--PC-relative-literal--
          FLAMINGO_ABORT("LDR of the SIMD variant is not yet supported!");
        } else {
          // This is an LDR that doesn't need to be fixed up
          FLAMINGO_DEBUG("Fixing up standard LDR...");
          Write(*reinterpret_cast<uint32_t const*>(inst.bytes));
        }
      } break;
      case ARM64_INS_LDRSW: {
        // This is an ldrsw literal
        // See TODOs for LDR
        // https://developer.arm.com/documentation/ddi0596/2021-12/Base-Instructions/LDRSW--literal---Load-Register-Signed-Word--literal--
        FLAMINGO_ABORT("LDRSW fixup not yet supported!");
      } break;

      // Handle pc-relative loads
      case ARM64_INS_ADR: {
        FLAMINGO_DEBUG("Fixing up ADR...");
        auto [reg, dst] = get_second_immediate(inst);
        WriteAdr(reg, dst);
      } break;
      case ARM64_INS_ADRP: {
        FLAMINGO_DEBUG("Fixing up ADRP...");
        auto [reg, dst] = get_second_immediate(inst);
        WriteAdrp(reg, dst);
      } break;

      // Otherwise, just write the instruction verbatim
      default:
        FLAMINGO_DEBUG("Fixing up UNKNOWN: {}...", inst.id);
        Write(*reinterpret_cast<uint32_t const*>(inst.bytes));
        break;
    }
  }
};
}  // namespace

namespace flamingo {

csh getHandle() {
  static csh handle = 0;
  static bool init = false;
  if (!init) {
    cs_err e1 = cs_open(CS_ARCH_ARM64, CS_MODE_ARM, &handle);
    cs_err e2 = cs_option(handle, CS_OPT_DETAIL, 1);
    if (e1 != CS_ERR_OK || e2 != CS_ERR_OK) {
      FLAMINGO_ABORT("Capstone initialization failed: {}, {}", static_cast<int>(e1), static_cast<int>(e2));
    }
    #ifdef GIT_COMMIT
    FLAMINGO_DEBUG("Hello from flamingo! Commit: {:#08x}", GIT_COMMIT);
    #else
    FLAMINGO_DEBUG("Hello from flamingo! Unknown build commit hash");
    #endif
    init = true;
  }
  return handle;
}

cs_insn debugInst(uint32_t const* inst) {
  cs_insn* insns = nullptr;
  auto count = cs_disasm(getHandle(), reinterpret_cast<uint8_t const*>(inst), sizeof(uint32_t),
                         static_cast<uint64_t>(get_untagged_pc(reinterpret_cast<uint64_t>(inst))), 1, &insns);
  if (count == 1) {
    return insns[0];
  }
  return {};
}

void ShimTarget::WriteJump(void* address) {
  FLAMINGO_ASSERT(!addr.empty());
  // TODO: We also want to correctly report if we were near! Because if so, then we DON'T actually need to fixup all N
  // instructions, but only need to do 1. So, the return from this should be a number of instructions that we will
  // DEFINITELY need to fixup for our actual fixups call (if we wish to create fixups)

  // The writer for ensuring correct permissions and also performing the write
  ProtectionWriter<uint32_t> writer(*this);
  WriteCallback(writer, reinterpret_cast<uint32_t*>(address));
}

void ShimTarget::WriteCallback(ProtectionWriter<uint32_t>& writer, uint32_t const* target) {
  constexpr uint32_t branch_imm_mask = 0b00000011111111111111111111111111U;
  auto delta = get_untagged_pc(target) - get_untagged_pc(&writer.target.addr[writer.target_offset]);
  if (std::llabs(delta) > (branch_imm_mask << 1) + 1) {
    // Too far for b. Emit a br instead.
    // TODO: Allow for different registers other than just x17
    // Note: If we change it here, we will also need to change it in the fixups too.
    constexpr uint32_t ldr_x17 = 0x58000051U;
    writer.Write(ldr_x17);
    constexpr uint32_t br_x17 = 0xD61F0220U;
    writer.Write(br_x17);
    // And write the target
    auto large_data = reinterpret_cast<uint64_t>(target);
    writer.Write(static_cast<uint32_t>(large_data & (UINT32_MAX)));
    writer.Write(static_cast<uint32_t>((large_data >> 32) & UINT32_MAX));
  } else {
    // Small enough to emit a b.
    // b opcode | encoded immediate (delta >> 2)
    // Note, abs(delta >> 2) must be < (1 << 26)
    constexpr uint32_t b_opcode = 0b00010100000000000000000000000000U;
    writer.Write(b_opcode | ((delta >> 2) & branch_imm_mask));
  }
}

void Fixups::CopyOriginalInsts() {
  FLAMINGO_ASSERT(!target.addr.empty());
  original_instructions.resize(target.addr.size());
  std::copy(target.addr.begin(), target.addr.end(), original_instructions.begin());
}

void Fixups::PerformFixupsAndCallback() {
  FLAMINGO_ASSERT(!target.addr.empty());
  FLAMINGO_ASSERT(!fixup_inst_destination.addr.empty());
  // As a precondition to this call, we must ensure we copied over the original instructions
  FLAMINGO_ASSERT(original_instructions.size() >= target.addr.size());
  // TODO: It is not thread safe to perform hooks on the same page as other threads!
  // This is because we could have a fixup writer complete on one thread after the other has started.
  // So, we want to lock on hook creation to ensure no one else is doing any type of hook creation, ideally.

  // Make the FixupContext instance that we will use for performing fixups
  FixupContext context(fixup_inst_destination, target.addr);

  // Now, for each instruction at target
  // Fix it up, maybe add an entry to the data block, maybe add an entry to the branch remapping
  // Then, after that, write our callback to target.addr
  // Finally, iterate our data ref tags and branch tags and edit the written fixups
  // Flush our icache and protection is restored by the ProtectionWriter instance getting dtor'd
  // The final layout should look something like:
  // - Instructions...
  // - Callback
  // - Data section...

  cs_insn* insns = nullptr;
  [[maybe_unused]] auto count = cs_disasm(
      flamingo::getHandle(), reinterpret_cast<uint8_t const*>(&target.addr[0]), target.addr.size_bytes(),
      static_cast<uint64_t>(get_untagged_pc(reinterpret_cast<uint64_t>(&target.addr[0]))), target.addr.size(), &insns);
  // We should never try to write fixups for something that isn't a valid instruction
  // However, sometimes capstone isn't the latest version or whatever, so we don't assert here
  // FLAMINGO_ASSERT(count == target.addr.size());

  for (uint_fast16_t i = 0; i < target.addr.size(); i++) {
    // For each input instruction, perform a fixup on it
    auto const& inst = insns[i];
    auto current_inst_ptr = &target.addr[i];
    FLAMINGO_DEBUG("Fixup for inst: 0x{:x} at {}: {} {}, id: {}", *current_inst_ptr, fmt::ptr(current_inst_ptr),
                   fmt::string_view(inst.mnemonic, sizeof(inst.mnemonic)),
                   fmt::string_view(inst.op_str, sizeof(inst.op_str)), static_cast<int>(inst.id));
    // For this incoming instruction, check to see if we have any forward references on this
    // If we do, for each, rewrite the target instruction with the adjusted value
    for (auto const& tag : context.branch_ref_map[i]) {
      // Current PC is GetFixupPC()
      // The instruction we emit's PC is the map from target --> fixup
      // This difference is always positive, since we are jumping FORWARD
      auto difference = static_cast<uint32_t>(context.GetFixupPC()) -
                        get_untagged_pc(reinterpret_cast<uint64_t>(
                            &fixup_inst_destination.addr[context.target_to_fixups[tag.target_index]]));
      FLAMINGO_DEBUG("Performing deferred write at: {}, rewriting: {} with difference: {}", i, tag.target_index,
                     difference);
      fixup_inst_destination.addr[context.target_to_fixups[tag.target_index]] =
          (fixup_inst_destination.addr[context.target_to_fixups[tag.target_index]] & ~tag.imm_mask) |
          (tag.imm_mask & ((difference >> tag.rshift) << tag.lshift));
    }
    context.PerformFixupFor(inst, i, current_inst_ptr);
  }

  // Free the disassembled instructions from before the fixups
  cs_free(insns, target.addr.size());
  // Now, write the callback after all of our fixups.
  context.WriteCallback(&target.addr[target.addr.size()]);
  // After we have written ALL of our fixups initially AND our callback, perform our second pass where we inject
  // immediate offsets. Most specifically, for data. To do this, we first start by laying out our data section directly,
  // and marking the start address as "base". Then, we compute offsets based off of base + data_index * sizeof(uint32_t)
  // - &fixups[fixup_idx]
  auto data_base = context.GetFixupPC();
  for (auto& data : context.data_block) {
    // Check our location for alignment
    auto const align_bytes = (data.alignment * sizeof(uint32_t));
    auto misalignment = context.GetFixupPC() % align_bytes;
    if (misalignment != 0) {
      FLAMINGO_DEBUG("MISALIGNED ADDRESS: {:#x} ALIGNING TO: {} REQUIRES: {} BYTES", context.GetFixupPC(), align_bytes,
                     (align_bytes - misalignment));
      // Need to write 0s to pad
      for (size_t i = 0; i < (align_bytes - misalignment); i += sizeof(uint32_t)) {
        context.Write(0U);
      }
    }
    data.actual_idx = (context.GetFixupPC() - data_base) / sizeof(uint32_t);
    context.Write(data.data);
  }
  for (auto const& tag : context.data_ref_tags) {
    auto const actual_data_idx = context.data_block[tag.data_index].actual_idx;
    int_fast16_t offset = static_cast<int_fast16_t>(data_base + actual_data_idx * sizeof(uint32_t) -
                                                    get_untagged_pc(&fixup_inst_destination.addr[tag.fixup_index]));
    FLAMINGO_DEBUG("ACTUAL DATA INDEX: {} FOR TAG AT FIXUP: {} OFFSET IN BYTES: {} AT: {}", actual_data_idx,
                   tag.fixup_index, offset, data_base + actual_data_idx * sizeof(uint32_t));
    fixup_inst_destination.addr[tag.fixup_index] = (fixup_inst_destination.addr[tag.fixup_index] & ~tag.imm_mask) |
                                                   (tag.imm_mask & ((offset >> tag.rshift) << tag.lshift));
  }
  // Flush the icache for our fixups in case they were already cached from another hook call
  __builtin___clear_cache(reinterpret_cast<char*>(&fixup_inst_destination.addr[0]),
                          reinterpret_cast<char*>(&fixup_inst_destination.addr[fixup_inst_destination.addr.size()]));
}

void Fixups::Log() const {
  // To log fixups, we walk the instructions and perform a translation for each
}

void Fixups::Uninstall() {
  // To perform an uninstall, we just iterate over all of our original instructions and copy them all back to the target
  ProtectionWriter<uint32_t> writer(target);
  for (auto const inst : original_instructions) {
    writer.Write(inst);
  }
}

// TODO: We should consider an optimization where we have a location for fixup data instead of inling all fixups.
// This would allow us to write out actual assembly verbatim and then have ldrs and whatnot for grabbing the data
// This would save a few instructions per all of the fixups, since we wouldn't need to branch over the data
// In addition, it would allow us to have a better time disassembling.
// It also shouldn't cost us any space whatsoever
// Though, if we perform any hooks AFTER the fact, we would need to properly expand both our data and our instruction
// space and THAT could be somewhat tricky. Perhaps a full recompile for a hook is actually preferred, though, if we
// know we need to leapfrog Since we would need to expand our trampoline and our original instructions regardless.
// TODO: Consider a full recompile and permit late installations

// If we have a fixup that has a ret, we need to avoid the callback after the ret potentially (though it would be
// redundant anyways)

}  // namespace flamingo
