#include "Utils.hpp"
extern "C" __attribute__((visibility("default"))) int sc2_test_beatsaver_safe_extract(const char *data,size_t size,const char *dest){
 return BeatSaver::Utils::ExtractAll(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(data),size),std::filesystem::path(dest))?0:1;
}
