// A C face on smol-v's decoder, for tools/unity_pipelines.py (ctypes).
#include "smolv.h"

extern "C" size_t kl_smolv_decoded_size(const void *data, size_t size) {
    return smolv::GetDecodedBufferSize(data, size);
}

// Decodes the SMOL-V program that starts at `data`; `size` may run past its
// end. Returns the bytes of input it took, or -1.
extern "C" long kl_smolv_decode(const void *data, size_t size, void *out, size_t out_size) {
    if (!smolv::Decode(data, size, out, out_size, smolv::kDecodeFlagNone)) return -1;
    return (long)smolv::g_klDecodedInputSize;
}
