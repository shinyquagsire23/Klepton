#pragma once

#include "./_config.h"
#include <string>
#include "beatsaber-hook/shared/utils/typedefs.h"

namespace BSML::DataCache {
    class Entry;
    /// @brief Registers an entry in the datacache.
    /// @param key the key to use in your BSML file if you want to use this data as a source
    /// @param value the entry to register
    BSML_EXPORT void RegisterEntry(std::string key, const Entry* value);

    class BSML_EXPORT Entry {
        public:
            Entry(std::string key) { BSML::DataCache::RegisterEntry(key, this); }
            virtual ArrayW<uint8_t> get_data() const = 0;
    };
}

/** define used by BSML to get data from your mod into a cache to be used by anything that requires access to data within a mod for things like images
 * Make sure it returns a valid ArrayW<uint8_t>
 * if you want to use the data registered this way in your BSML file, use it like this:
 *
 * say my mod is Qosmetics, and uses the MOD_ID Qosmetics.
 * if I wanted to use a data key, it'd look like this in my BSML file: Qosmetics_sabericon
 * sabericon is what I typed into the BSML_DATACACHE macro as you can see below:
 *
 * BSML_DATACACHE(sabericon) {
 *     return IncludedAssets::sabericon_png;
 * }
 *
 * the keys are prepended with your MOD_ID to prevent key collisions with other mods
 */
#define BSML_DATACACHE(key)\
class BSMLDataCacheEntry_##key : public BSML::DataCache::Entry {\
    public:\
        BSMLDataCacheEntry_##key() : BSML::DataCache::Entry(MOD_ID "_" #key) {}\
        ArrayW<uint8_t> get_data() const override;\
};\
static const BSMLDataCacheEntry_##key BSMLDataCacheEntry_##key##_instance{};\
ArrayW<uint8_t> BSMLDataCacheEntry_##key::get_data() const
