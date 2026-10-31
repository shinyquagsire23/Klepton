#pragma once

#include "../../_config.h"
#include "VRUIControls/VRPointer.hpp"
#include "UnityEngine/Vector3.hpp"
#include "UnityEngine/Quaternion.hpp"

namespace BSML {
    struct BSML_EXPORT FloatingScreenHandleEventArgs {
        FloatingScreenHandleEventArgs(::VRUIControls::VRPointer* vrPointer, UnityEngine::Vector3 position, UnityEngine::Quaternion rotation) : vrPointer(vrPointer), position(position), rotation(rotation) {}
        const SafePtrUnity<::VRUIControls::VRPointer> vrPointer;
        const UnityEngine::Vector3 position;
        const UnityEngine::Quaternion rotation;

        bool operator==(const FloatingScreenHandleEventArgs& other) const {
            return (
                position.x == other.position.x &&
                position.y == other.position.y &&
                position.z == other.position.z
            ) && (
                rotation.x == other.rotation.x &&
                rotation.y == other.rotation.y &&
                rotation.z == other.rotation.z &&
                rotation.w == other.rotation.w
            );
        }

        bool operator!=(const FloatingScreenHandleEventArgs& other) const {
            return !(*this == other);
        }
    };
}
