// Stamp file used when building for Android (to ensure we pull in the static library correctly)
#include <dlfcn.h>
#include <cstddef>
#include <cstdint>
#include "fixups.hpp"
#include "git_info.inc"
#include "hook-data.hpp"
#include "installer.hpp"
#include "target-data.hpp"
#include "util.hpp"

extern void* modloader_libil2cpp_handle;

void* (*orig_runtime_invoke)(void*, void*, void*, void*);

void* wrap_runtime_invoke(void* method_info, void* obj, void* p, void* e) {
  FLAMINGO_DEBUG("Runtime Invoke: {}, {}, {}", method_info, obj, p);
  return orig_runtime_invoke(method_info, obj, p, e);
}

static void print_decode_loop(void* ptr, size_t size) {
  auto handle = flamingo::getHandle();
  uint32_t* data = (uint32_t*)ptr;
  for (size_t i = 0; i < size; i++) {
    cs_insn* insns = nullptr;
    auto count = cs_disasm(handle, reinterpret_cast<uint8_t const*>(&data[i]), sizeof(uint32_t),
                           reinterpret_cast<uint64_t>(&data[i]), 1, &insns);
    if (count == 1) {
      FLAMINGO_DEBUG("Addr: {} Value: 0x{:08x}, {} {}", fmt::ptr(&data[i]), data[i], insns[0].mnemonic,
                     insns[0].op_str);
    } else {
      FLAMINGO_DEBUG("Addr: {} Value: 0x{:08x}", fmt::ptr(&data[i]), data[i]);
    }
  }
}

FLAMINGO_EXPORT extern "C" void late_load() {
  FLAMINGO_DEBUG("GIT COMMIT: 0x{:08x}", GIT_COMMIT);
  void* (*runtime_invoke)(void*, void*, void*, void*);
  runtime_invoke = (decltype(runtime_invoke))dlsym(modloader_libil2cpp_handle, "il2cpp_runtime_invoke");
  FLAMINGO_DEBUG("Found runtime_invoke: {}", fmt::ptr(runtime_invoke));
  print_decode_loop((void*)runtime_invoke, 10);
  // Read the b at this location and get it back out
  {
    constexpr uint64_t mask = ~(0xFFULL << (64U - 8U));
    auto pc = static_cast<int64_t>(reinterpret_cast<uint64_t>(runtime_invoke) & mask);
    cs_insn* insns = nullptr;
    cs_disasm(flamingo::getHandle(), reinterpret_cast<uint8_t const*>(runtime_invoke), 4, pc, 1, &insns);
    FLAMINGO_DEBUG("inst ops: {}", (int)insns->detail->arm64.op_count);
    runtime_invoke = (decltype(runtime_invoke))insns->detail->arm64.operands[0].imm;
    FLAMINGO_DEBUG("runtime_invoke after b resolved: {}", fmt::ptr(runtime_invoke));
  }

  print_decode_loop((void*)runtime_invoke, 10);
  // Install the hook to it
  auto result =
      flamingo::Install(flamingo::HookInfo(&wrap_runtime_invoke, (void*)runtime_invoke, &orig_runtime_invoke));
  if (!result.has_value()) {
    FLAMINGO_ABORT("Hook installation error! Error is of type: {}", result.error());
  }
  // After hook install, log the hook
  FLAMINGO_DEBUG("runtime_invoke again: {}", fmt::ptr(runtime_invoke));
  FLAMINGO_DEBUG("Target hook addr: {}", fmt::ptr(&wrap_runtime_invoke));
  FLAMINGO_DEBUG("Orig callback (should match runtime_invoke): {}", fmt::ptr(orig_runtime_invoke));
  print_decode_loop((void*)runtime_invoke, 10);
  // Ask for the fixups location and dump that
  auto fixups = flamingo::FixupPointerFor(flamingo::TargetDescriptor{ .target = (void*)runtime_invoke });
  if (!result.has_value()) {
    FLAMINGO_ABORT("Fixup lookup failed for target: {}", (void*)runtime_invoke);
  }
  FLAMINGO_DEBUG("Fixups pointer: {}", fmt::ptr(fixups.value().data()));
  print_decode_loop((void*)fixups.value().data(), fixups.value().size());
  FLAMINGO_DEBUG("ALL SET!");
}
