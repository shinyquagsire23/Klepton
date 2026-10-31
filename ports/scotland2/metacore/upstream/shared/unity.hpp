#pragma once

#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Quaternion.hpp"
#include "UnityEngine/Sprite.hpp"
#include "UnityEngine/Texture2D.hpp"
#include "UnityEngine/Transform.hpp"
#include "UnityEngine/Vector3.hpp"
#include "export.h"

#if __has_include("BSML/shared/Helpers/utilities.hpp")
#include "BSML/shared/Helpers/utilities.hpp"
#endif

namespace MetaCore::Engine {
    template <typename T>
    concept has_transform = !std::is_convertible_v<T, UnityEngine::Transform*> && requires(T t) {
        { t->transform } -> std::convertible_to<UnityEngine::Transform*>;
    };

    /// @brief A wrapper to automatically convert Unity objects into their transforms
    struct TransformWrapper {
        constexpr TransformWrapper(UnityEngine::Transform* transform) noexcept : transform(transform) {}

        template <typename T>
        requires(std::is_convertible_v<T, UnityEngine::Transform*>)
        TransformWrapper(T t) : TransformWrapper(static_cast<UnityEngine::Transform*>(t)) {}

        template <has_transform T>
        TransformWrapper(T t) : TransformWrapper(t->transform) {}

        UnityEngine::Transform* operator->() const noexcept { return transform; }
        operator UnityEngine::Transform*() const noexcept { return transform; }
        UnityEngine::Transform* transform;
    };

    /// @brief Gets the euler angles of a Quaternion, with each axis clamped between -180 and 180
    /// @param rotation The rotation to use
    /// @return The clamped euler angles
    METACORE_EXPORT UnityEngine::Vector3 GetClampedEuler(UnityEngine::Quaternion rotation);

    /// @brief Recursively disables all objects with a whitelist and blacklist
    /// @param parent The top of the hierarchy to disable
    /// @param enabled The names of GameObjects to leave enabled (and all of their parents)
    /// @param disabled The names of GameObjects to disable without checking any of their children for the whitelist
    METACORE_EXPORT void DisableAllBut(TransformWrapper parent, std::set<std::string> enabled, std::set<std::string> disabled = {});
    /// @brief Recursively searches for a child, breadth first
    /// @param parent The top of the hierarchy to search
    /// @param name The name of the child to search for
    /// @return The child transform, or nullptr if not found
    METACORE_EXPORT UnityEngine::Transform* FindRecursive(TransformWrapper parent, std::string name);
    /// @brief Finds the path from a parent object to one of its children
    /// @param parent The starting point of the path in the hierarchy
    /// @param child The ending point of the path in the hierarchy
    /// @return The names of the objects between parent and (including) child separated by "/", or an empty string if child is not a child of parent
    METACORE_EXPORT std::string GetTransformPath(TransformWrapper parent, TransformWrapper child);
    /// @brief Places a one child object a specific sibling amount away from another of its siblings
    /// @param child The object to move in the hierarchy
    /// @param ref The sibling to move child relative to
    /// @param amount The nonzero distance from ref for child to be set to
    METACORE_EXPORT void SetRelativeSiblingIndex(TransformWrapper child, TransformWrapper ref, int amount);

    /// @brief Gets a component on an object, adding a new one if not present
    /// @tparam T The component type
    /// @tparam O The object type
    /// @param object The object the component should be on
    /// @return The component on the object
    template <class T, class O>
    T GetOrAddComponent(O object) {
        if (T existing = object->template GetComponent<T>())
            return existing;
        return object->gameObject->template AddComponent<T>();
    }

    /// @brief Scales a texture to a new size, using basic interpolation
    /// @param texture The original texture
    /// @param width The width for the output, or -1 to leave unchanged
    /// @param height The height for the output, or -1 to leave unchanged
    /// @param bounds The section of the texture to scale, or the full texture if width or height is 0
    /// @return The pixels of the scaled texture
    METACORE_EXPORT ArrayW<UnityEngine::Color> ScalePixels(UnityEngine::Texture2D* texture, int width, int height, UnityEngine::Rect bounds = {});
    /// @brief Scales the texture of a sprite to a new size, using basic interpolation
    /// @param texture The original texture
    /// @param width The width for the output, or -1 to leave unchanged
    /// @param height The height for the output, or -1 to leave unchanged
    /// @param bounds The section of the texture to scale, or the full texture if width or height is 0
    /// @return The pixels of the scaled sprite texture
    inline ArrayW<UnityEngine::Color> ScalePixels(UnityEngine::Sprite* sprite, int width, int height) {
        return ScalePixels(sprite->texture, width, height, sprite->textureRect);
    }

    /// @brief Scales a texture to a new size, using basic interpolation
    /// @param texture The original texture
    /// @param width The width for the output, or -1 to leave unchanged
    /// @param height The height for the output, or -1 to leave unchanged
    /// @param bounds The section of the texture to scale, or the full texture if width or height is 0
    /// @return The scaled texture
    METACORE_EXPORT UnityEngine::Texture2D* ScaleTexture(UnityEngine::Texture2D* texture, int width, int height, UnityEngine::Rect bounds = {});
    /// @brief Scales the texture of a sprite to a new size, using basic interpolation
    /// @param texture The original texture
    /// @param width The width for the output, or -1 to leave unchanged
    /// @param height The height for the output, or -1 to leave unchanged
    /// @param bounds The section of the texture to scale, or the full texture if width or height is 0
    /// @return The scaled sprite texture
    inline UnityEngine::Texture2D* ScaleTexture(UnityEngine::Sprite* sprite, int width, int height) {
        return ScaleTexture(sprite->texture, width, height, sprite->textureRect);
    }

#if __has_include("BSML/shared/Helpers/utilities.hpp")
    /// @brief Scales the texture of a sprite to a new size, using basic interpolation
    /// @param sprite The sprite to modify
    /// @param width The width for the output, or -1 to leave unchanged
    /// @param height The height for the output, or -1 to leave unchanged
    /// @return The scaled sprite
    inline UnityEngine::Sprite* ScaleSprite(UnityEngine::Sprite* sprite, int width, int height) {
        return BSML::Utilities::LoadSpriteFromTexture(ScaleTexture(sprite, width, height), sprite->pixelsPerUnit);
    }
#endif

    /// @brief Writes a texture to a file
    /// @param texture The texture to write
    /// @param file The path of the destination file
    /// @param bounds The section of the texture to write, or the full texture if width or height is 0
    METACORE_EXPORT void WriteTexture(UnityEngine::Texture2D* texture, std::string file, UnityEngine::Rect bounds = {});
    /// @brief Writes the texture of a sprite to a file
    /// @param sprite The sprite to write the texture of
    /// @param file The path of the destination file
    METACORE_EXPORT void WriteSprite(UnityEngine::Sprite* sprite, std::string file);

    /// @brief Schedules a function to be run on the main thread
    /// @param callback The function to be run on the main thread
    METACORE_EXPORT void ScheduleMainThread(std::function<void()> callback);
    /// @brief Schedules a function to be run on the main thread once a condition returns true
    /// @param wait A function that returns true once the callback should be run
    /// @param callback The function to be run once the condition is true
    METACORE_EXPORT void ScheduleMainThread(std::function<bool()> wait, std::function<void()> callback);

    /// @brief Sets a function to be run when a given object is enabled (via Unity's OnEnable callback)
    /// @param object The object to attach the callback to
    /// @param callback The callback to run when the object is enabled
    /// @param once If the callback should only run once then be removed
    METACORE_EXPORT void SetOnEnable(TransformWrapper object, std::function<void()> callback, bool once = false);
    /// @brief Sets a function to be run when a given object is disabled (via Unity's OnDisable callback)
    /// @param object The object to attach the callback to
    /// @param callback The callback to run when the object is disabled
    /// @param once If the callback should only run once then be removed
    METACORE_EXPORT void SetOnDisable(TransformWrapper object, std::function<void()> callback, bool once = false);
    /// @brief Sets a function to be run when a given object is destroyed (via Unity's OnDestroy callback)
    /// @param object The object to attach the callback to
    /// @param callback The callback to run when the object is destroyed
    METACORE_EXPORT void SetOnDestroy(TransformWrapper object, std::function<void()> callback);

    /// @brief Schedules a function to be run every frame for the rest of the application
    /// @param callback The function to be run every frame
    METACORE_EXPORT void ScheduleOnUpdate(std::function<void()> callback);

    /// @brief A struct to calculate the average of a number of rotations
    struct QuaternionAverage {
       public:
        /// @brief Creates a new QuaternionAverage instance
        /// @param baseRot The first rotation to include in the average
        /// @param ignoreY If rotation on the Y axis should be left out of the average
        QuaternionAverage(UnityEngine::Quaternion baseRot, bool ignoreY) : baseRotation(baseRot), ignoreY(ignoreY) {}

        /// @brief Adds a new rotation to the average
        /// @param rot The rotation to add
        METACORE_EXPORT void AddRotation(UnityEngine::Quaternion rot);
        /// @brief Gets the average rotation for this instance
        /// @return The current average of all added rotations
        METACORE_EXPORT UnityEngine::Quaternion GetAverage() const;

       private:
        UnityEngine::Quaternion cumulative = {0, 0, 0, 0};
        int num = 0;
        UnityEngine::Quaternion const baseRotation;
        bool const ignoreY;
    };
}
