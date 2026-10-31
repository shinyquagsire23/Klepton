#pragma once
#include <stdint.h>
#include <stddef.h>
struct mach_header_64;
// The translated image is already mapped by dyld. Only the prepared data slots
// are written. Absence of hook metadata is a successful no-op.
int kl_aot_attach(const struct mach_header_64 *header, const void *elf);
void kl_aot_detach(const struct mach_header_64 *header);
void *kl_aot_original(const void *target);
int kl_aot_exchange(void *target, void *replacement, void **previous);
int kl_aot_restore(void *target);
const char *kl_aot_error(void);
int kl_aot_error_code(void);
int kl_aot_entry_word(const void *target,uint32_t *word);
void *kl_aot_current(const void *target);
void kl_aot_begin(void);
void kl_aot_end(void);
// Process-lifetime native dispatch functions may register their already-signed
// body and writable dispatch slot. Both must remain mapped for the process.
// This never patches native/system executable text.
int kl_aot_register_native_gate(void *target, uintptr_t *slot, void *original);
// Code inspection sees original entry instructions at their original PCs.
// With output NULL only count overrides; otherwise copy then restore data words.
size_t kl_aot_copy_code(const void *source,void *output,size_t size);
