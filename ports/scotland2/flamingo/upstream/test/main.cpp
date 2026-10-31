// Main test runner for testing fixups behave as intended
#include <capstone/arm64.h>
#include <fmt/compile.h>
#include <fmt/core.h>
#include <fmt/format.h>
#include <string.h>
#include <sys/mman.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>

#include "../shared/fixups.hpp"
#include "../shared/page-allocator.hpp"
#include "capstone/capstone.h"
#include "test-wrapper.hpp"

static decltype(auto) test_near(std::span<uint32_t> target, [[maybe_unused]] uint32_t const* callback) {
  constexpr size_t hookSizeNumInsts = 5;
  constexpr size_t trampolineSize = 32;
  // First, perform a near allocation to the allocated location
  auto near_data = alloc_near(target, trampolineSize);
  fmt::println("NEAR TRAMPOLINE RESULT: {}", fmt::ptr(near_data.fixups.data()));
  flamingo::Fixups fixups{
    .target = { flamingo::PointerWrapper<uint32_t>{
      std::span(near_data.target.begin(), near_data.target.begin() + hookSizeNumInsts - 1),
      flamingo::PageProtectionType::kExecute | flamingo::PageProtectionType::kRead |
          flamingo::PageProtectionType::kWrite } },
    .fixup_inst_destination = flamingo::PointerWrapper<uint32_t>(
        near_data.fixups, flamingo::PageProtectionType::kExecute | flamingo::PageProtectionType::kRead |
                              flamingo::PageProtectionType::kWrite),
  };
  fixups.CopyOriginalInsts();
  fixups.PerformFixupsAndCallback();
  return fixups;
}

static auto perform_near_hook_test(std::span<uint8_t> to_hook) {
  std::span<uint32_t> hook_span = std::span<uint32_t>(reinterpret_cast<uint32_t*>(&to_hook[0]),
                                                      reinterpret_cast<uint32_t*>(&to_hook[to_hook.size()]));
  printf("TO HOOK: %p\n", to_hook.data());
  print_decode_loop(hook_span);
  puts("TEST NEAR...");
  auto trampoline_data = test_near(hook_span, (uint32_t const*)(0xDEADBEEFBAADF00DULL));
  // Use 20 here as a reasonable guesstimate
  print_decode_loop(trampoline_data.fixup_inst_destination.addr);
  puts("HOOKED:");
  print_decode_loop(trampoline_data.target.addr);
  return trampoline_data;
}

static decltype(auto) test_far(std::span<uint32_t> target, [[maybe_unused]] uint32_t const* callback) {
  constexpr size_t hookSizeNumInsts = 5;
  constexpr size_t trampolineSize = 32;
  // We allocate the page with r-x perms, we will mark it as writable when we do the writes and otherwise put it back to
  // this state.
  auto fixup_ptr = flamingo::Allocate(16, trampolineSize * sizeof(uint32_t),
                                      flamingo::PageProtectionType::kExecute | flamingo::PageProtectionType::kRead);
  // To ensure we test far hooks correctly, we copy from target to a page allocated far from fixup_ptr
  auto actual_target = alloc_far(fixup_ptr, target);
  fmt::println("FAR TRAMPOLINE RESULT: {}", fmt::ptr(actual_target.data()));
  flamingo::Fixups fixups{
    .target = { flamingo::PointerWrapper<uint32_t>{
      std::span(actual_target.begin(), actual_target.begin() + hookSizeNumInsts - 1),
      flamingo::PageProtectionType::kExecute | flamingo::PageProtectionType::kRead } },
    .fixup_inst_destination = fixup_ptr,
  };
  printf("TRAMPOLINE: %p\n", &fixup_ptr.addr[0]);
  // Attempt to write a hook from target --> callback (just for testing purposes)
  // Hook size is 5, but we only fixup 4
  fixups.CopyOriginalInsts();
  fixups.PerformFixupsAndCallback();
  return fixups;
}

static auto perform_far_hook_test(std::span<uint8_t> to_hook) {
  std::span<uint32_t> hook_span = std::span<uint32_t>(reinterpret_cast<uint32_t*>(&to_hook[0]),
                                                      reinterpret_cast<uint32_t*>(&to_hook[to_hook.size()]));
  printf("TO HOOK: %p\n", to_hook.data());
  print_decode_loop(hook_span);
  puts("TEST FAR...");
  auto fixups = test_far(hook_span, (uint32_t const*)(0xDEADBEEFBAADF00DULL));
  // Use 20 here as a reasonable guesstimate
  print_decode_loop(fixups.fixup_inst_destination.addr);
  puts("HOOKED:");
  print_decode_loop(fixups.target.addr);
  return fixups;
}

static void test_no_fixups() {
  puts("Testing no fixups!");
  static uint8_t to_hook[]{ 0xf7, 0x0f, 0x1c, 0xf8, 0xf6, 0x57, 0x01, 0xa9, 0xf4, 0x4f, 0x02, 0xa9, 0xfd, 0x7b, 0x03,
                            0xa9, 0xfd, 0xc3, 0x00, 0x91, 0x48, 0x18, 0x40, 0xf9, 0x16, 0xd4, 0x42, 0xa9, 0xf3, 0x03,
                            0x02, 0xaa, 0xf4, 0x03, 0x01, 0xaa, 0x17, 0x01, 0x40, 0xf9, 0xe8, 0xba, 0x44, 0x39 };
  {
    TestWrapper init_hook(to_hook, "No fixups initial data");
    init_hook.expect_opc(ARM64_INS_STR);
    init_hook.expect_opc(ARM64_INS_STP);
    init_hook.expect_opc(ARM64_INS_STP);
    init_hook.expect_opc(ARM64_INS_STP);
    init_hook.expect_opc(ARM64_INS_ADD);
  }
  {
    auto results = perform_near_hook_test(to_hook);
    TestWrapper fixup_validator(results.fixup_inst_destination.addr, "Near hook no fixups");
    fixup_validator.expect_opc(ARM64_INS_STR);
    fixup_validator.expect_opc(ARM64_INS_STP);
    fixup_validator.expect_opc(ARM64_INS_STP);
    fixup_validator.expect_opc(ARM64_INS_STP);
    // Callback
    fixup_validator.expect_b(&results.target.addr[4]);
  }
  {
    auto results = perform_far_hook_test(to_hook);
    TestWrapper fixup_validator(results.fixup_inst_destination.addr, "Far hook no fixups");
    fixup_validator.expect_opc(ARM64_INS_STR);
    fixup_validator.expect_opc(ARM64_INS_STP);
    fixup_validator.expect_opc(ARM64_INS_STP);
    fixup_validator.expect_opc(ARM64_INS_STP);
    // Callback (ldr x17, DATA[0]; br x17)
    fixup_validator.expect_ops<ARM64_OP_REG, ARM64_OP_IMM>(ARM64_INS_LDR, ARM64_REG_X17,
                                                           round_up8(&results.fixup_inst_destination.addr[6]));
    fixup_validator.expect_ops<ARM64_OP_REG>(ARM64_INS_BR, ARM64_REG_X17);
    // Data validation
    // Check callback point is valid
    fixup_validator.expect_big_data(reinterpret_cast<uint64_t>(&results.target.addr[4]));
  }
}

static void test_bls_tbzs_within_hook() {
  puts("Testing bls/tbzs");
  static uint8_t to_hook[]{ 0x68, 0x00, 0x00, 0x37, 0xe0, 0x03, 0x17, 0xaa, 0x52, 0x3e, 0xfd, 0x97,
                            0xe0, 0x03, 0x17, 0xaa, 0x64, 0x7b, 0xfe, 0x97, 0x00, 0x00, 0x00, 0x00 };
  {
    TestWrapper init_hook(to_hook, "bls/tbzs");
    init_hook.expect_ops<ARM64_OP_REG, ARM64_OP_IMM, ARM64_OP_IMM>(ARM64_INS_TBNZ, ARM64_REG_W8, 0,
                                                                   (int64_t)&init_hook.data[3]);
    init_hook.expect_opc(ARM64_INS_MOV);
    init_hook.expect_opc(ARM64_INS_BL);
    init_hook.expect_opc(ARM64_INS_MOV);
    init_hook.expect_opc(ARM64_INS_BL);
  }
  {
    auto results = perform_near_hook_test(to_hook);
    TestWrapper fixup_validator(results.fixup_inst_destination.addr, "Near hook bls/tbzs");
    fixup_validator.expect_ops<ARM64_OP_REG, ARM64_OP_IMM, ARM64_OP_IMM>(
        ARM64_INS_TBNZ, ARM64_REG_W8, 0, (int64_t)&results.fixup_inst_destination.addr[3]);
    fixup_validator.expect_opc(ARM64_INS_MOV);
    fixup_validator.expect_opc(ARM64_INS_BL);
    fixup_validator.expect_opc(ARM64_INS_MOV);
    // Callback
    fixup_validator.expect_b(&results.target.addr[4]);
  }
  {
    auto results = perform_far_hook_test(to_hook);
    TestWrapper fixup_validator(results.fixup_inst_destination.addr, "Far hook bls/tbzs");
    // tbnz is still close, should still be emitted, but should still point to the mov, which is at idx 4
    fixup_validator.expect_ops<ARM64_OP_REG, ARM64_OP_IMM, ARM64_OP_IMM>(
        ARM64_INS_TBNZ, ARM64_REG_W8, 0, (int64_t)&results.fixup_inst_destination.addr[4]);
    // mov is the same
    fixup_validator.expect_opc(ARM64_INS_MOV);
    // bl should turn into an ldr x17, DATA[0]; blr x17
    fixup_validator.expect_ops<ARM64_OP_REG, ARM64_OP_IMM>(ARM64_INS_LDR, ARM64_REG_X17,
                                                           round_up8(&results.fixup_inst_destination.addr[7]));
    fixup_validator.expect_ops<ARM64_OP_REG>(ARM64_INS_BLR, ARM64_REG_X17);
    fixup_validator.expect_opc(ARM64_INS_MOV);
    // Callback (ldr x17, DATA[1]; br x17)
    fixup_validator.expect_ops<ARM64_OP_REG, ARM64_OP_IMM>(ARM64_INS_LDR, ARM64_REG_X17,
                                                           round_up8(&results.fixup_inst_destination.addr[9]));
    fixup_validator.expect_ops<ARM64_OP_REG>(ARM64_INS_BR, ARM64_REG_X17);
    // Data validation
    // The branch destination should be -0xB06B0 relative to the start of the hook.
    // Thus: target == reinterpret_cast<uint64_t>(&
    fixup_validator.expect_big_data(reinterpret_cast<uint64_t>(results.target.addr.data()) - 0xB06B0);
    // Check callback point is valid
    fixup_validator.expect_big_data(reinterpret_cast<uint64_t>(&results.target.addr[4]));
  }
}

static void test_ldr_ldrb_tbnz_bl() {
  puts("Testing ldr/ldrb/tbnz/bl");
  static uint8_t to_hook[]{ 0x17, 0x01, 0x40, 0xf9, 0xe8, 0xba, 0x44, 0x39, 0x68, 0x00, 0x00, 0x37,
                            0xe0, 0x03, 0x17, 0xaa, 0x52, 0x3e, 0xfd, 0x97, 0x00, 0x00, 0x00, 0x00 };
  {
    TestWrapper init_hook(to_hook, "ldr/ldrb/tbnz/bl");
    init_hook.expect_opc(ARM64_INS_LDR);
    init_hook.expect_opc(ARM64_INS_LDRB);
    init_hook.expect_ops<ARM64_OP_REG, ARM64_OP_IMM, ARM64_OP_IMM>(ARM64_INS_TBNZ, ARM64_REG_W8, 0,
                                                                   (int64_t)&init_hook.data[5]);
    init_hook.expect_opc(ARM64_INS_MOV);
  }
  {
    auto results = perform_near_hook_test(to_hook);
    TestWrapper fixup_validator(results.fixup_inst_destination.addr, "Near hook ldr/ldrb/tbnz/bl");
    fixup_validator.expect_opc(ARM64_INS_LDR);
    fixup_validator.expect_opc(ARM64_INS_LDRB);
    // TBNZ should jump straight to the hook location if taken
    // TODO: This test should change once we support near tbz/tbnz optimizations
    fixup_validator.expect_ops<ARM64_OP_REG, ARM64_OP_IMM, ARM64_OP_IMM>(ARM64_INS_TBNZ, ARM64_REG_W8, 0,
                                                                         (int64_t)&results.target.addr[5]);
    fixup_validator.expect_opc(ARM64_INS_MOV);
    // Callback
    fixup_validator.expect_b(&results.target.addr[4]);
    // Data validation
  }
  {
    auto results = perform_far_hook_test(to_hook);
    TestWrapper fixup_validator(results.fixup_inst_destination.addr, "Far hook ldr/ldrb/tbnz/bl");
    fixup_validator.expect_opc(ARM64_INS_LDR);
    fixup_validator.expect_opc(ARM64_INS_LDRB);
    // TBNZ should jump over the following instruction if taken
    fixup_validator.expect_ops<ARM64_OP_REG, ARM64_OP_IMM, ARM64_OP_IMM>(
        ARM64_INS_TBNZ, ARM64_REG_W8, 0, (int64_t)&results.fixup_inst_destination.addr[4]);
    // B instruction should jump to skip the following far branch call
    fixup_validator.expect_b(&results.fixup_inst_destination.addr[6]);
    // Far branch call is given by an ldr x17, DATA[0]; br x17
    fixup_validator.expect_ops<ARM64_OP_REG, ARM64_OP_IMM>(ARM64_INS_LDR, ARM64_REG_X17,
                                                           round_up8(&results.fixup_inst_destination.addr[9]));
    fixup_validator.expect_ops<ARM64_OP_REG>(ARM64_INS_BR, ARM64_REG_X17);
    fixup_validator.expect_opc(ARM64_INS_MOV);
    // Callback (ldr x17, DATA[1]; br x17)
    fixup_validator.expect_ops<ARM64_OP_REG, ARM64_OP_IMM>(ARM64_INS_LDR, ARM64_REG_X17,
                                                           round_up8(&results.fixup_inst_destination.addr[11]));
    fixup_validator.expect_ops<ARM64_OP_REG>(ARM64_INS_BR, ARM64_REG_X17);
    // Data validation
    // Branch destination for tbnz taken should be hook[5]
    fixup_validator.expect_big_data((uint64_t)&results.target.addr[5]);
    // Check callback point is valid
    fixup_validator.expect_big_data(reinterpret_cast<uint64_t>(&results.target.addr[4]));
  }
}

static void test_adrp() {
  puts("Testing adrp");
  static uint8_t to_hook[]{ 0x09, 0x00, 0x00, 0x90, 0xa8, 0x00, 0x80, 0x52, 0x28, 0x01,
                            0x00, 0xb9, 0x28, 0x01, 0x00, 0xb9, 0xc0, 0x03, 0x5f, 0xd6 };
  {
    TestWrapper init_hook(to_hook, "adrp");
    init_hook.expect_ops<ARM64_OP_REG, ARM64_OP_IMM>(ARM64_INS_ADRP, ARM64_REG_X9, (int64_t)(&to_hook[0]) & ~0xfff);
    init_hook.expect_opc(ARM64_INS_MOV);
    init_hook.expect_opc(ARM64_INS_STR);
    init_hook.expect_opc(ARM64_INS_STR);
    init_hook.expect_opc(ARM64_INS_RET);
  }
  {
    auto results = perform_near_hook_test(to_hook);
    TestWrapper fixup_validator(results.fixup_inst_destination.addr, "Near hook adrp");
    // ADRP is replaced with an ldr to load the data directly
    // LDR x9, DATA[0]
    fixup_validator.expect_ops<ARM64_OP_REG, ARM64_OP_IMM>(ARM64_INS_LDR, ARM64_REG_X9,
                                                           round_up8(&results.fixup_inst_destination.addr[5]));
    fixup_validator.expect_opc(ARM64_INS_MOV);
    fixup_validator.expect_opc(ARM64_INS_STR);
    fixup_validator.expect_opc(ARM64_INS_STR);
    // Callback
    fixup_validator.expect_b(&results.target.addr[4]);
    // Data validation
    // ADRP result must match
    fixup_validator.expect_big_data((int64_t)(results.target.addr.data()) & ~0xfff);
  }
  {
    auto results = perform_far_hook_test(to_hook);
    TestWrapper fixup_validator(results.fixup_inst_destination.addr, "Far hook adrp");
    // ADRP is replaced with an ldr to load the data directly
    // LDR x9, DATA[0]
    fixup_validator.expect_ops<ARM64_OP_REG, ARM64_OP_IMM>(ARM64_INS_LDR, ARM64_REG_X9,
                                                           round_up8(&results.fixup_inst_destination.addr[6]));
    fixup_validator.expect_opc(ARM64_INS_MOV);
    fixup_validator.expect_opc(ARM64_INS_STR);
    fixup_validator.expect_opc(ARM64_INS_STR);
    // Callback (ldr x17, DATA[1]; br x17)
    fixup_validator.expect_ops<ARM64_OP_REG, ARM64_OP_IMM>(ARM64_INS_LDR, ARM64_REG_X17,
                                                           round_up8(&results.fixup_inst_destination.addr[8]));
    fixup_validator.expect_ops<ARM64_OP_REG>(ARM64_INS_BR, ARM64_REG_X17);
    // ADRP result must match
    fixup_validator.expect_big_data((int64_t)(results.target.addr.data()) & ~0xfff);
    // Check callback point is valid
    fixup_validator.expect_big_data(reinterpret_cast<uint64_t>(&results.target.addr[4]));
  }
}

static void test_neg_adrp() {
  puts("Testing negative adrp");
  static uint8_t to_hook[]{ 0x1f, 0x20, 0x00, 0x71, 0xa8, 0x00, 0x00, 0x54, 0xc8, 0x58, 0xff, 0x90,
                            0x08, 0xc1, 0x29, 0x91, 0x00, 0xd9, 0x60, 0xb8, 0xc0, 0x03, 0x5f, 0xd6 };
  {
    TestWrapper init_hook(to_hook, "negative adrp");
    init_hook.expect_opc(ARM64_INS_CMP);
    init_hook.expect_opc(ARM64_INS_B);
    init_hook.expect_ops<ARM64_OP_REG, ARM64_OP_IMM>(ARM64_INS_ADRP, ARM64_REG_X8,
                                                     ((int64_t)(&to_hook[0]) - 0x14E8000) & ~0xfff);
    init_hook.expect_opc(ARM64_INS_ADD);
    init_hook.expect_opc(ARM64_INS_LDR);
  }
  {
    auto results = perform_near_hook_test(to_hook);
    TestWrapper fixup_validator(results.fixup_inst_destination.addr, "Near hook negative adrp");
    fixup_validator.expect_opc(ARM64_INS_CMP);
    // B is a near branch in this case
    fixup_validator.expect_opc(ARM64_INS_B);
    // ADRP is replaced with an ldr to load the data directly
    // LDR x8, DATA[0]
    fixup_validator.expect_ops<ARM64_OP_REG, ARM64_OP_IMM>(ARM64_INS_LDR, ARM64_REG_X8,
                                                           round_up8(&results.fixup_inst_destination.addr[6]));
    fixup_validator.expect_opc(ARM64_INS_ADD);
    // Callback
    fixup_validator.expect_b(&results.target.addr[4]);
    // Data validation
    // ADRP result must match
    fixup_validator.expect_big_data(((int64_t)(results.target.addr.data()) - 0x14E8000) & ~0xfff);
  }
  {
    auto results = perform_far_hook_test(to_hook);
    TestWrapper fixup_validator(results.fixup_inst_destination.addr, "Far hook negative adrp");
    fixup_validator.expect_opc(ARM64_INS_CMP);
    // b.hi past following b to ldr + br pair
    fixup_validator.expect_b(&results.fixup_inst_destination.addr[3]);
    // b over ldr + br pair
    fixup_validator.expect_b(&results.fixup_inst_destination.addr[5]);
    // LDR x17, DATA[0]
    fixup_validator.expect_ops<ARM64_OP_REG, ARM64_OP_IMM>(ARM64_INS_LDR, ARM64_REG_X17,
                                                           round_up8(&results.fixup_inst_destination.addr[10]));
    fixup_validator.expect_ops<ARM64_OP_REG>(ARM64_INS_BR, ARM64_REG_X17);
    // ADRP is replaced with an ldr to load the data directly
    // LDR x9, DATA[1]
    fixup_validator.expect_ops<ARM64_OP_REG, ARM64_OP_IMM>(ARM64_INS_LDR, ARM64_REG_X8,
                                                           round_up8(&results.fixup_inst_destination.addr[12]));
    fixup_validator.expect_opc(ARM64_INS_ADD);
    // Callback
    fixup_validator.expect_ops<ARM64_OP_REG, ARM64_OP_IMM>(ARM64_INS_LDR, ARM64_REG_X17,
                                                           round_up8(&results.fixup_inst_destination.addr[14]));
    fixup_validator.expect_ops<ARM64_OP_REG>(ARM64_INS_BR, ARM64_REG_X17);
    // Data validation
    // B.hi destination should match
    fixup_validator.expect_big_data((uint64_t)&results.target.addr[6]);
    // ADRP result must match
    fixup_validator.expect_big_data(((int64_t)(results.target.addr.data()) - 0x14E8000) & ~0xfff);
    // Check callback point is valid
    fixup_validator.expect_big_data(reinterpret_cast<uint64_t>(&results.target.addr[4]));
  }
}

// TODO: Test a case where we have a loop in the first 4 instructions
// TODO: Test a case where we have an ldr literal that loads from within fixup range
// TODO: Test a case with a negative adrp offset

int main() {
  test_no_fixups();
  test_bls_tbzs_within_hook();
  test_ldr_ldrb_tbnz_bl();
  test_adrp();
  test_neg_adrp();
  puts("ALL GOOD!");
}
