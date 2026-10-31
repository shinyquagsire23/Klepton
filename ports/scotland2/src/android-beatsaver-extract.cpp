// Preserve the released BeatSaverPlusPlus module. Adapt only its unsafe archive
// write boundary. C++ span/path objects stay between NDK-built guest modules;
// the Apple host installs this already-signed pointer through a C-only API.
#include <span>
#include <filesystem>
#include <cstdint>
extern "C" int zip_stream_extract(const char*,size_t,const char*,int (*)(const char*,void*),void*);
extern "C" __attribute__((visibility("default")))
bool sc2_beatsaver_extract(std::span<const uint8_t> data,const std::filesystem::path& output) {
    return zip_stream_extract(reinterpret_cast<const char*>(data.data()),data.size(),output.c_str(),nullptr,nullptr)==0;
}
