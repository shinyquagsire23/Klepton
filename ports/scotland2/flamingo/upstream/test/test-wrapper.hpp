#pragma once

#include <asm-generic/mman-common.h>
#include <fmt/compile.h>
#include <fmt/core.h>
#include <fmt/format.h>
#include <linux/mman.h>
#include <sys/mman.h>
#include <sys/user.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include "capstone/arm64.h"
#include "capstone/capstone.h"

#include "../shared/fixups.hpp"

// TODO: Only do a dump if an error happens
// Dump should do a normal decode loop:
// - Dump the original instructions for the hook
// - Dump the trampoline
// - Dump the new hook
// Perhaps we make TestWrapper take a Fixups instance for this? That way we can do all three of those.
#define ERROR(S, ...)                              \
  fmt::print(stderr, FMT_COMPILE(S), __VA_ARGS__); \
  fmt::print(stderr, "\n");                        \
  std::exit(1);

// Converts a pointer to data into the next 64b multiple for use with tests with differing alignments.
int64_t round_up8(auto* ptr) {
  auto value = reinterpret_cast<int64_t>(ptr);
  if ((value % 8) != 0) {
    return value + 4;
  }
  return value;
}

// TODO: ALSO ADD A MMAP WRAPPER TO GUARANTEE FAR HOOKS ARE FAR
// Helper construct to validate data from a hooked target
struct TestWrapper {
  std::span<uint32_t const> data;
  uint32_t idx{ 0 };
  std::string test_name;
  TestWrapper(std::span<uint32_t const> bytes, std::string_view test) : data(bytes), test_name(test) {
    start_test();
  }
  TestWrapper(std::span<uint8_t> bytes, std::string_view test)
      : data(std::span<uint32_t>(reinterpret_cast<uint32_t*>(&bytes[0]),
                                 reinterpret_cast<uint32_t*>(&bytes[bytes.size()]))),
        test_name(test) {
    start_test();
  }
  ~TestWrapper() {
    fmt::print("---Passed test: {}\n", test_name);
    fflush(stdout);
  }

  void start_test() const {
    fmt::print("---Starting test: {}\n", test_name);
    fflush(stdout);
  }

  cs_insn* get_next() {
    auto handle = flamingo::getHandle();
    cs_insn* insns = nullptr;
    auto count = cs_disasm(handle, reinterpret_cast<uint8_t const*>(&data[idx]), sizeof(uint32_t),
                           reinterpret_cast<uint64_t>(&data[idx]), 1, &insns);
    idx++;
    if (count == 1) {
      // We just leak this, who cares.
      return insns;
    }
    // Failed to disassemble!
    return nullptr;
  }
  uint32_t get_next_data() {
    return data[idx++];
  }
  uint64_t get_next_big_data() {
    // If &data[idx] is not aligned 64b, we need to increment index first
    if ((reinterpret_cast<uint64_t>(&data[idx]) % 8) != 0) {
      idx++;
    }
    uint64_t value = static_cast<uint64_t>(get_next_data());
    value |= static_cast<uint64_t>(get_next_data()) << 32U;
    return value;
  }

  void expect_inst_opc(cs_insn* inst, unsigned int opcode) const {
    if (inst == nullptr) {
      ERROR("Mismatched instruction at index: {}\n Expected opcode: {}\n Got: Invalid instruction", idx - 1, opcode);
    }
    if (inst->id != opcode) {
      ERROR("Mismatched instruction at index: {}\n Expected opcode: {}\n Got: {}", idx - 1, opcode, inst->id);
    }
  }
  void expect_opc(unsigned int opcode) {
    auto inst = get_next();
    expect_inst_opc(inst, opcode);
    cs_free(inst, 1);
  }
  void expect_b(uint32_t* addr) {
    auto inst = get_next();
    expect_inst_opc(inst, ARM64_INS_B);
    if (inst->detail->arm64.operands[0].imm != reinterpret_cast<int64_t>(addr)) {
      ERROR("Mismatched B at index: {}\n Expected immediate: {}\n Got: {:#x}", idx - 1, fmt::ptr(addr),
            inst->detail->arm64.operands[0].imm);
    }
    cs_free(inst, 1);
  }
  template <arm64_op_type OpType, class T>
  static bool compare_op(cs_arm64_op const& op, T value) {
    if (op.type != OpType) return false;
    switch (OpType) {
      case arm64_op_type::ARM64_OP_IMM:
        return op.imm == value;
      case arm64_op_type::ARM64_OP_REG:
        return op.reg == value;
      default:
        ERROR("Cannot compare operand of type: {}", static_cast<int>(OpType));
    }
  }
  template <arm64_op_type... OpTypes, size_t... Sz, class... TArgs>
  void validate_ops(std::index_sequence<Sz...>, cs_arm64_op* ops, std::tuple<TArgs...> expected) {
    auto expect_op = [&]<arm64_op_type OpType, size_t Size>() {
      if (!compare_op<OpType>(ops[Size], std::get<Size>(expected))) {
        ERROR("Mismatched instruction at index: {} Mismatched operand at index: {}\n Expected: {}: {}\n Got: {}: {}",
              idx, Size, static_cast<int64_t>(OpType), static_cast<int64_t>(std::get<Size>(expected)),
              static_cast<int64_t>(ops[Size].type), ops[Size].imm);
      }
    };
    (expect_op.template operator()<OpTypes, Sz>(), ...);
  }
  template <arm64_op_type... OpTypes, class... TArgs>
  void expect_ops(unsigned int opcode, TArgs&&... args) {
    auto inst = get_next();
    expect_inst_opc(inst, opcode);
    auto opcount = inst->detail->arm64.op_count;
    static_assert(sizeof...(OpTypes) == sizeof...(TArgs), "Must have a type for each operand");
    if (sizeof...(args) > opcount) {
      ERROR("Mismatched instruction at index: {}\n Expected opcount: {}\n Got: {}", idx - 1, sizeof...(args), opcount);
    }
    // Index wrapper to validate each operand in order
    validate_ops<OpTypes...>(std::make_index_sequence<sizeof...(args)>{}, inst->detail->arm64.operands,
                             std::make_tuple(std::forward<TArgs>(args)...));
    cs_free(inst, 1);
  }
  void expect_data(uint32_t expected) {
    auto data = get_next_data();
    if (data != expected) {
      ERROR("Mismatched 32b data at index: {}\n Expected: {}\n Got: {}", idx - 1, expected, data);
    }
  }
  void expect_big_data(uint64_t expected) {
    auto data = get_next_big_data();
    if (data != expected) {
      ERROR("Mismatched 64b data at index: {}\n Expected: {}\n Got: {}", idx - 1, expected, data);
    }
  }
};

struct TestAllocResult {
  std::span<uint32_t> target;
  std::span<uint32_t> fixups;
};

inline auto alloc_near(std::span<uint32_t> target, size_t fixup_size) {
  // Alloc 2 pages so we can remap target too
  auto result = mmap(nullptr, PAGE_SIZE * 2, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (result == MAP_FAILED) {
    perror(strerror(errno));
  }
  std::memcpy(result, target.data(), target.size_bytes());
  return TestAllocResult{
    .target = std::span<uint32_t>((uint32_t*)result, (uint32_t*)result + target.size()),
    .fixups = std::span<uint32_t>((uint32_t*)result + PAGE_SIZE / sizeof(uint32_t),
                                  (uint32_t*)result + PAGE_SIZE / sizeof(uint32_t) + fixup_size),
  };
}

inline auto alloc_far(flamingo::PointerWrapper<uint32_t> target_fixups, std::span<uint32_t> data_to_copy) {
  // Continue fetching pages until we find one that's far enough away from target_fixups
  // TODO: Cache the found page and give it back each time we call this function
  // but I don't care enough for that
  constexpr auto kPageCount = 16384;
  constexpr auto kRange = 0x800000LL;
  for (int i = 0; i < kPageCount; i++) {
    auto result = mmap(nullptr, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (result == MAP_FAILED) {
      perror(strerror(errno));
      break;
    }
    if (std::llabs((int64_t)result - (int64_t)target_fixups.addr.data()) > kRange) {
      // Found a page we can use
      std::memcpy(result, data_to_copy.data(), data_to_copy.size_bytes());
      // Return the new result as a span
      return std::span<uint32_t>((uint32_t*)result, (uint32_t*)result + data_to_copy.size());
    }
  }
  // If we can't find ANY pages that match this criteria even after trying a bunch, abort
  ERROR("Could not find any pages (tried: {}) that are outside of the range: {} of: {}", kPageCount, kRange,
        fmt::ptr(target_fixups.addr.data()));
}

inline void print_decode_loop(std::span<uint32_t const> data) {
  auto handle = flamingo::getHandle();
  for (size_t i = 0; i < data.size(); i++) {
    cs_insn* insns = nullptr;
    auto count = cs_disasm(handle, reinterpret_cast<uint8_t const*>(&data[i]), sizeof(uint32_t),
                           reinterpret_cast<uint64_t>(&data[i]), 1, &insns);
    if (count == 1) {
      printf("Addr: %p Value: 0x%08x, %s %s\n", &data[i], data[i], insns[0].mnemonic, insns[0].op_str);
    } else {
      printf("Addr: %p Value: 0x%08x\n", &data[i], data[i]);
    }
  }
}
