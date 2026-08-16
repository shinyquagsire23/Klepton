// KL_DUMP_METADATA=<path> — write the guest's DECRYPTED global-metadata.dat out
// of its own memory, after il2cpp_init has run.
//
// It exists for VRChat, and for every target that ever protects its metadata the
// same way. VRChat's `global-metadata.dat` on disk begins `0xad6b5f05` instead of
// IL2CPP's `0xFAB11BAF`: the string table is plaintext (so every type name and
// source path is greppable on disk) and the index tables are
// not, so nothing on disk maps a name to a method or a method to an address. The
// usual second door is shut too — `libil2cpp.so` has 2241 dynamic symbols and
// exactly three matching `il2cpp_*`, the rest renamed, which is the real reason
// kl_mprobe cannot attach to this target.
//
// But il2cpp itself has to read the tables, so the plaintext exists in this
// process once init has run, and this is our process. Scanning for the magic and
// writing the region out hands Il2CppDumper the whole thing: method names, field
// names, and addresses in libil2cpp.
//
// Diagnostic only, host-only, and it does not run unless asked.
#ifndef KL_METADUMP_H
#define KL_METADUMP_H

#include <stddef.h>
#include <stdio.h>

// Scan this process for a decrypted IL2CPP metadata blob and write it to the
// path in KL_DUMP_METADATA. Does nothing if the variable is unset. Returns the
// number of bytes written, or 0.
//
// `disk_metadata` is the guest's own `global-metadata.dat` on disk, or NULL. It
// is not where the answer comes from — it is what makes the answer checkable. A
// protected header cannot be measured, because the offset pairs a plain header
// is measured by are ciphertext, so for a blob carrying the guest's own magic
// the file supplies the only remaining statement about its extent (its length)
// and the only evidence that a candidate is plaintext at all (that it DIFFERS
// from the file). Without it a 35 MB copy of the ciphertext, sitting exactly
// where it was read, would be reported as the dump.
size_t kl_metadump_run(const char *disk_metadata);

// KL_META_WATCH=1 — name every instruction that READS the metadata mapping.
//
// The dump above only works for a target that decrypts its metadata into one
// plaintext image. VRChat does not, so the algorithm has to be read out of the
// code — and that code is not reachable statically: its runtime `.text` makes
// no direct PLT calls at all. This finds it instead by taking the mapping away
// with mprotect and letting the readers announce themselves as faults.
//
// Install before the guest boots; the report is a census of reader sites, each
// as `<image>+0x<off>` with the metadata offset it first touched.
void kl_metadump_watch_install(void);
void kl_metadump_watch_report(FILE *out);

#endif
