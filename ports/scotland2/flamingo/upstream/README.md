# Flamingo
An Android inline hooking library with priorities, dynamic trampoline allocation, and optimizations.

## Partial Todo

- Priority based hooks (before/after in constexpr fashion)
- Hook handler to avoid redundant assembly, potentially dynamically realloc
- Hook creation via varying types of installs (delayed, instant, etc.)
- Support optimizations for functions that are normally too small
- Support recompilation of fixup trampoline
- Easily obtain hooks in read-only context for third party API use
- Avoid dependencies on beatsaber-hook entirely
- Support trampoline allocations in non-static context, ensure alignment and instruction flushing
- Make fern happy
