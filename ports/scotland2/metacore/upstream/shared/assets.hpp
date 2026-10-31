#pragma once

#include "beatsaber-hook/shared/utils/typedefs.h"

/// @brief A struct that holds references to an asset and provides accessors to its data
struct IncludedAsset {
    IncludedAsset(uint8_t* start, uint8_t* end) : array((Array<uint8_t>*) start) {
        array->klass = nullptr;
        array->monitor = nullptr;
        array->bounds = nullptr;
        array->max_length = end - start - 32;
        *(end - 1) = '\0';
    }

    operator ArrayW<uint8_t>() const {
        init();
        return array;
    }
    operator std::string_view() const { return {(char*) array->_values, array->max_length}; }
    operator std::span<uint8_t>() const { return {array->_values, array->max_length}; }

    size_t size() const { return array->max_length; }
    void* data() const { return (void*) array->_values; }

   private:
    void init() const {
        if (!array->klass)
            array->klass = classof(Array<uint8_t>*);
    }
    Array<uint8_t>* const array;
};

#if __has_include("bsml/shared/Helpers/utilities.hpp")
#include "bsml/shared/Helpers/utilities.hpp"

#define PNG_SPRITE(name) \
    BSML::Utilities::LoadSpriteRaw(static_cast<ArrayW<uint8_t>>(IncludedAssets::name##_png))
#endif
