// IL2CPP pc -> managed method name resolver (Unity 2019.4, metadata v24).
//
// Turns a guest pc inside libil2cpp.so's generated code into
// "Namespace.Type::Method", using the same structures Il2CppDumper reads
// statically: global-metadata.dat for method/type names, and libil2cpp's
// Il2CppCodeGenModule table for the method-index -> code-pointer mapping.
// The table is not exported, so init scans the loaded image for it and
// validates every candidate against the metadata (see kl_il2cpp.c).
#ifndef KL_IL2CPP_H
#define KL_IL2CPP_H

#include "klepton.h"

// One-time build of the resolver. `libil2cpp` is the loaded guest image,
// `metadata_path` the host path to global-metadata.dat. Returns 1 when at
// least one codegen module was recovered. Cheap to call again: later calls
// return the first result.
int kl_il2cpp_resolver_init(kl_image *libil2cpp, const char *metadata_path);

// Managed method containing `pc`, as an interned "Namespace.Type::Method"
// string, or NULL when the resolver is not up or the pc is not in any known
// method. The pointer stays valid for the life of the process.
const char *kl_il2cpp_method_at(const void *pc);

// How many methods the resolver knows (0 when not initialised).
unsigned kl_il2cpp_method_count(void);

#endif
