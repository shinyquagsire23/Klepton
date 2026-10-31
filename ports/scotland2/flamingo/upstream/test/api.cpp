#include <cstdint>
#include <span>
#include <utility>
#include "calling-convention.hpp"
#include "hook-data.hpp"
#include "hook-metadata.hpp"
#include "installer.hpp"
#include "page-allocator.hpp"
#include "target-data.hpp"
#include "test-wrapper.hpp"

namespace {

auto perform_far_hook_test(uintptr_t hook_location, std::span<uint8_t> to_hook) {
  std::span<uint32_t> hook_span = std::span<uint32_t>(reinterpret_cast<uint32_t*>(&to_hook[0]),
                                                      reinterpret_cast<uint32_t*>(&to_hook[to_hook.size()]));
  fmt::println("TO HOOK: {}", fmt::ptr(hook_span.data()));
  print_decode_loop(hook_span);
  // Give me back a pointer to some "far" allocated region that we can touch
  return alloc_far(flamingo::PointerWrapper<uint32_t>(std::span<uint32_t>(reinterpret_cast<uint32_t*>(hook_location),
                                                                          reinterpret_cast<uint32_t*>(hook_location)),
                                                      flamingo::PageProtectionType::kNone),
                   hook_span);
}

void test_simple_hook() {
  // Boilerplate for the test wrapper
  uintptr_t hook_function_to_call = 0x12345678;
  static uint8_t to_hook[]{ 0xf7, 0x0f, 0x1c, 0xf8, 0xf6, 0x57, 0x01, 0xa9, 0xf4, 0x4f, 0x02, 0xa9, 0xfd, 0x7b, 0x03,
                            0xa9, 0xfd, 0xc3, 0x00, 0x91, 0x48, 0x18, 0x40, 0xf9, 0x16, 0xd4, 0x42, 0xa9, 0xf3, 0x03,
                            0x02, 0xaa, 0xf4, 0x03, 0x01, 0xaa, 0x17, 0x01, 0x40, 0xf9, 0xe8, 0xba, 0x44, 0x39 };
  // Simplest hook (no orig, num instructions is default, Cdecl calling convention, "" name, no priorities,
  // non-midpoint)
  // Validate initial function looks good
  {
    TestWrapper init_hook(to_hook, "No fixups initial data");
    init_hook.expect_opc(ARM64_INS_STR);
    init_hook.expect_opc(ARM64_INS_STP);
    init_hook.expect_opc(ARM64_INS_STP);
    init_hook.expect_opc(ARM64_INS_STP);
    init_hook.expect_opc(ARM64_INS_ADD);
  }
  auto hook_target_far = perform_far_hook_test(hook_function_to_call, to_hook);
  auto result = flamingo::Install(
      flamingo::HookInfo{ (void (*)())hook_function_to_call, hook_target_far.data(), (void (**)()) nullptr });
  if (!result.has_value()) {
    ERROR("Installation result failed, index: {}", result.error());
  }
  // Validate target looks good (should call hook_function_to_call)
  {
    TestWrapper validator(hook_target_far, "Far hook no fixups");
    print_decode_loop(hook_target_far);
    // Callback (ldr x17, DATA[0]; br x17)
    validator.expect_ops<ARM64_OP_REG, ARM64_OP_IMM>(ARM64_INS_LDR, ARM64_REG_X17,
          round_up8(&hook_target_far[2]));
    validator.expect_ops<ARM64_OP_REG>(ARM64_INS_BR, ARM64_REG_X17);
    // Data validation
    // Check callback point is valid
    validator.expect_big_data(hook_function_to_call);
  }
  // Uninstall the hook and ensure the data returns to its natural state
  {
    auto uninstall_result = flamingo::Uninstall(result.value().returned_handle);
    if (!uninstall_result.has_value()) {
      ERROR("Failed to uninstall: failure mode: {}", uninstall_result.error());
    }
    if (uninstall_result.value() == true) {
      ERROR("Uninstall should have wiped this target clean, since there is only one hook, but didn't!? Target: {}", fmt::ptr(hook_target_far.data()));
    }
    TestWrapper validate_uninstall(hook_target_far, "After uninstall, return to original");
    print_decode_loop(hook_target_far);
    validate_uninstall.expect_opc(ARM64_INS_STR);
    validate_uninstall.expect_opc(ARM64_INS_STP);
    validate_uninstall.expect_opc(ARM64_INS_STP);
    validate_uninstall.expect_opc(ARM64_INS_STP);
    validate_uninstall.expect_opc(ARM64_INS_ADD);
  }
}

void test_hook_with_orig() {
  // Boilerplate for the test wrapper
  uintptr_t hook_function_to_call = 0x12345678;
  void* fixup_result_ptr;
  static uint8_t to_hook[]{ 0xf7, 0x0f, 0x1c, 0xf8, 0xf6, 0x57, 0x01, 0xa9, 0xf4, 0x4f, 0x02, 0xa9, 0xfd, 0x7b, 0x03,
                            0xa9, 0xfd, 0xc3, 0x00, 0x91, 0x48, 0x18, 0x40, 0xf9, 0x16, 0xd4, 0x42, 0xa9, 0xf3, 0x03,
                            0x02, 0xaa, 0xf4, 0x03, 0x01, 0xaa, 0x17, 0x01, 0x40, 0xf9, 0xe8, 0xba, 0x44, 0x39 };
  // Simplest hook but with an orig (num instructions is default, Cdecl calling convention, "" name, no priorities,
  // non-midpoint)
  // Validate initial function looks good
  {
    TestWrapper init_hook(to_hook, "No fixups initial data");
    init_hook.expect_opc(ARM64_INS_STR);
    init_hook.expect_opc(ARM64_INS_STP);
    init_hook.expect_opc(ARM64_INS_STP);
    init_hook.expect_opc(ARM64_INS_STP);
    init_hook.expect_opc(ARM64_INS_ADD);
  }
  auto hook_target_far = perform_far_hook_test(hook_function_to_call, to_hook);
  auto result = flamingo::Install(
      flamingo::HookInfo{ (void (*)())hook_function_to_call, hook_target_far.data(), (void (**)()) &fixup_result_ptr });
  if (!result.has_value()) {
    ERROR("Installation result failed, index: {}", result.error().index());
  }
  // Query the fixups we would have written and validate those are good
  {
    auto fixup_result = flamingo::FixupPointerFor(flamingo::TargetDescriptor(hook_target_far.data()));
    if (!fixup_result.has_value()) {
      ERROR("Failed to get fixup pointer for target: {}", fmt::ptr(hook_target_far.data()));
    }
    TestWrapper fixups(fixup_result.value(), "Fixup data");
    print_decode_loop(fixup_result.value());
    fixups.expect_opc(ARM64_INS_STR);
    fixups.expect_opc(ARM64_INS_STP);
    fixups.expect_opc(ARM64_INS_STP);
    fixups.expect_opc(ARM64_INS_STP);
    // Callback (ldr x17, DATA[0]; br x17)
    fixups.expect_ops<ARM64_OP_REG, ARM64_OP_IMM>(ARM64_INS_LDR, ARM64_REG_X17,
          round_up8(&fixup_result.value()[6]));
    fixups.expect_ops<ARM64_OP_REG>(ARM64_INS_BR, ARM64_REG_X17);
    // Data validation
    // Check callback point is valid
    fixups.expect_big_data(reinterpret_cast<uint64_t>(&hook_target_far[4]));
  }
}


void test_multi_hook() {
  // Boilerplate for the test wrapper
  // Keep these values close together so that the far hook destination is far for both of them.
  uintptr_t hook_function_to_call = 0x12345678;
  uintptr_t hook_function_to_call_2 = 0x12345679;
  void* fixup_result_ptr;
  void* orig_two;
  static uint8_t to_hook[]{ 0xf7, 0x0f, 0x1c, 0xf8, 0xf6, 0x57, 0x01, 0xa9, 0xf4, 0x4f, 0x02, 0xa9, 0xfd, 0x7b, 0x03,
                            0xa9, 0xfd, 0xc3, 0x00, 0x91, 0x48, 0x18, 0x40, 0xf9, 0x16, 0xd4, 0x42, 0xa9, 0xf3, 0x03,
                            0x02, 0xaa, 0xf4, 0x03, 0x01, 0xaa, 0x17, 0x01, 0x40, 0xf9, 0xe8, 0xba, 0x44, 0x39 };
  // Simplest hook (no orig, num instructions is default, Cdecl calling convention, "" name, no priorities,
  // non-midpoint)
  auto hook_target_far = perform_far_hook_test(hook_function_to_call, to_hook);
  auto result = flamingo::Install(
    flamingo::HookInfo{ (void (*)())hook_function_to_call, hook_target_far.data(), (void (**)()) &fixup_result_ptr });
  if (!result.has_value()) {
    ERROR("Installation result failed, index: {}", result.error().index());
  }
  // Second hook
  auto second_hook = flamingo::Install(
    flamingo::HookInfo{ (void (*)())hook_function_to_call_2, hook_target_far.data(), (void (**)())&orig_two}
  );
  if (!result.has_value()) {
    ERROR("Installation result for hook 2 failed, index: {}", result.error().index());
  }
  // Validate target looks good (should call hook_function_to_call_2)
  {
    TestWrapper validator(hook_target_far, "Far hook no fixups");
    print_decode_loop(hook_target_far);
    // Callback (ldr x17, DATA[0]; br x17)
    validator.expect_ops<ARM64_OP_REG, ARM64_OP_IMM>(ARM64_INS_LDR, ARM64_REG_X17,
          round_up8(&hook_target_far[2]));
    validator.expect_ops<ARM64_OP_REG>(ARM64_INS_BR, ARM64_REG_X17);
    // Data validation
    // Check callback point is valid
    validator.expect_big_data(hook_function_to_call_2);
  }
  // Hook 2's orig pointer should refer to hook 1's target
  if ((uintptr_t)orig_two != hook_function_to_call) {
    ERROR("Hook 2 should call hook 1 as part of hook 2's orig call! Instead, hook 2's orig is: 0x{:x}", (uintptr_t)orig_two);
  }
  // Now, uninstall hook 1. Hook 2's orig pointer should then refer to the fixup pointer
  auto uninstall_result = flamingo::Uninstall(result.value().returned_handle);
  if (!uninstall_result.has_value()) {
    ERROR("Failed to uninstall: failure mode: {}", uninstall_result.error());
  }
  if (uninstall_result.value() == false) {
    ERROR("Uninstall should NOT have wiped this target clean, since there is still one hook left. Target: {}", fmt::ptr(hook_target_far.data()));
  }
  // Get orig pointer for the hook and compare it to orig_two's held pointer now
  auto fixup_result = flamingo::FixupPointerFor(flamingo::TargetDescriptor(hook_target_far.data()));
  if (!fixup_result.has_value()) {
    ERROR("Failed to get fixup pointer for target: {}", fmt::ptr(hook_target_far.data()));
  }
  if ((uintptr_t)orig_two != (uintptr_t)fixup_result.value().data()) {
    ERROR("Hook 2 should fixups for the target as part of hook 2's orig call! Instead, hook 2's orig is: 0x{:x} and the fixups are: {}", (uintptr_t)orig_two, fmt::ptr(fixup_result.value().data()));
  }
}
}  // namespace

int main() {
  test_simple_hook();
  test_hook_with_orig();
  test_multi_hook();
}
