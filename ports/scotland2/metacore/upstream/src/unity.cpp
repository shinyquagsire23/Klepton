#include "unity.hpp"

#include "UnityEngine/Color.hpp"
#include "UnityEngine/Graphics.hpp"
#include "UnityEngine/ImageConversion.hpp"
#include "UnityEngine/Rect.hpp"
#include "UnityEngine/RenderTexture.hpp"
#include "UnityEngine/RenderTextureFormat.hpp"
#include "UnityEngine/RenderTextureReadWrite.hpp"
#include "UnityEngine/TextureFormat.hpp"
#include "beatsaber-hook/shared/utils/il2cpp-utils.hpp"
#include "main.hpp"
#include "operators.hpp"
#include "types.hpp"

using namespace UnityEngine;

Vector3 MetaCore::Engine::GetClampedEuler(Quaternion rotation) {
    auto ret = rotation.eulerAngles;
    for (auto val : {&ret.x, &ret.y, &ret.z}) {
        if (*val > 180)
            *val -= 360;
    }
    return ret;
}

static void DisableAllButImpl(Transform* original, Transform* source, std::set<std::string> enabled, std::set<std::string> disabled) {
    for (int i = 0; i < source->GetChildCount(); i++) {
        auto child = source->GetChild(i).unsafePtr();
        std::string name = child->name;
        if (enabled.contains(name)) {
            auto loopback = child;
            while (loopback != original) {
                loopback->gameObject->active = true;
                loopback = loopback->parent;
            }
        } else {
            child->gameObject->active = false;
            if (!disabled.contains(name))
                DisableAllButImpl(original, child, enabled, disabled);
        }
    }
}

void MetaCore::Engine::DisableAllBut(TransformWrapper parent, std::set<std::string> enabled, std::set<std::string> disabled) {
    if (!enabled.contains(parent->name))
        DisableAllButImpl(parent, parent, enabled, disabled);
}

Transform* MetaCore::Engine::FindRecursive(TransformWrapper parent, std::string name) {
    for (int i = 0; i < parent->GetChildCount(); i++) {
        auto child = parent->GetChild(i);
        if (child->name == name)
            return child;
    }
    // breadth first
    for (int i = 0; i < parent->GetChildCount(); i++) {
        if (auto ret = FindRecursive(parent->GetChild(i), name))
            return ret;
    }
    return nullptr;
}

std::string MetaCore::Engine::GetTransformPath(TransformWrapper parent, TransformWrapper child) {
    if (parent == child || !child->IsChildOf(parent))
        return "";
    auto parents = GetTransformPath(parent, child->parent);
    if (!parents.empty())
        return fmt::format("{}/{}", parents, child->name);
    return child->name;
}

void MetaCore::Engine::SetRelativeSiblingIndex(TransformWrapper child, TransformWrapper ref, int amount) {
    // zero won't crash or anything, I just think it's a little confusing in its behavior
    if (amount == 0 || !child->parent || child->parent != ref->parent)
        return;
    int currentIndex = child->GetSiblingIndex();
    int otherIndex = ref->GetSiblingIndex();
    // adjust for moving around if after -> before or before -> after
    // (unity child order is weird and I don't like it)
    if (currentIndex < otherIndex && amount > 0)
        amount--;
    else if (currentIndex > otherIndex && amount < 0)
        amount++;
    child->SetSiblingIndex(otherIndex + amount);
}

static Texture2D* GetReadable(Texture2D* texture, Rect bounds) {
    if (texture->isReadable)
        return texture;

    auto ret = Texture2D::New_ctor(bounds.m_Width, bounds.m_Height, texture->format, false, false);

    auto temp = UnityEngine::RenderTexture::GetTemporary(
        texture->width, texture->height, 0, UnityEngine::RenderTextureFormat::Default, UnityEngine::RenderTextureReadWrite::Default
    );
    UnityEngine::Graphics::Blit(texture, temp);
    ret->ReadPixels(bounds, 0, 0);
    UnityEngine::RenderTexture::ReleaseTemporary(temp);

    return ret;
}

ArrayW<Color> MetaCore::Engine::ScalePixels(Texture2D* texture, int width, int height, Rect bounds) {
    int origWidth = texture->width;

    if (bounds.m_Width <= 0 || bounds.m_Height <= 0)
        bounds = {0, 0, (float) origWidth, (float) texture->height};

    if (width <= 0)
        width = bounds.m_Width;
    if (height <= 0)
        height = bounds.m_Height;

    ArrayW<Color> origColors = GetReadable(texture, bounds)->GetPixels();
    ArrayW<Color> destColors(width * height);

    float ratioX = (texture->width - 1) / (float) width;
    float ratioY = (texture->height - 1) / (float) height;

    for (int destY = 0; destY < height; destY++) {
        int origY = (int) destY * ratioY;
        float yLerp = destY * ratioY - origY;

        float yIdx1 = origY * origWidth;
        float yIdx2 = (origY + 1) * origWidth;
        float yIdxDest = destY * width;

        for (int destX = 0; destX < width; destX++) {
            int origX = (int) destX * ratioX;
            float xLerp = destX * ratioX - origX;
            destColors[yIdxDest + destX] = Color::Lerp(
                Color::Lerp(origColors[yIdx1 + origX], origColors[yIdx1 + origX + 1], xLerp),
                Color::Lerp(origColors[yIdx2 + origX], origColors[yIdx2 + origX + 1], xLerp),
                yLerp
            );
        }
    }

    return destColors;
}

Texture2D* MetaCore::Engine::ScaleTexture(Texture2D* texture, int width, int height, Rect bounds) {
    auto pixels = ScalePixels(texture, width, height, bounds);
    auto ret = Texture2D::New_ctor(width, height, TextureFormat::RGBA32, false, false);
    ret->SetPixels(pixels);
    ret->Apply();
    return ret;
}

void MetaCore::Engine::WriteTexture(Texture2D* texture, std::string file, Rect bounds) {
    auto bytes = ImageConversion::EncodeToPNG(GetReadable(texture, bounds));
    writefile(file, std::string((char*) bytes.begin(), bytes->get_Length()));
}

void MetaCore::Engine::WriteSprite(Sprite* sprite, std::string file) {
    WriteTexture(sprite->texture, file, sprite->textureRect);
}

void MetaCore::Engine::ScheduleMainThread(std::function<void()> callback) {
    MainThreadScheduler::Schedule(std::move(callback));
}

void MetaCore::Engine::ScheduleMainThread(std::function<bool()> wait, std::function<void()> callback) {
    MainThreadScheduler::Schedule(std::move(wait), std::move(callback));
}

void MetaCore::Engine::SetOnEnable(TransformWrapper object, std::function<void()> callback, bool once) {
    auto signal = GetOrAddComponent<ObjectSignal*>(object);
    if (once)
        callback = [signal, callback = std::move(callback)]() {
            callback();
            signal->onEnable = nullptr;
        };
    signal->onEnable = std::move(callback);
}

void MetaCore::Engine::SetOnDisable(TransformWrapper object, std::function<void()> callback, bool once) {
    auto signal = GetOrAddComponent<ObjectSignal*>(object);
    if (once)
        callback = [signal, callback = std::move(callback)]() {
            callback();
            signal->onDisable = nullptr;
        };
    signal->onDisable = std::move(callback);
}

void MetaCore::Engine::SetOnDestroy(TransformWrapper object, std::function<void()> callback) {
    ObjectSignal::onDestroys[object->gameObject->GetInstanceID()] = callback;
}

void MetaCore::Engine::ScheduleOnUpdate(std::function<void()> callback) {
    MainThreadScheduler::AddUpdate(std::move(callback));
}

// math from https://stackoverflow.com/a/20249699
void MetaCore::Engine::QuaternionAverage::AddRotation(Quaternion rot) {
    // remove y rotation from average on 360 degree levels
    if (ignoreY) {
        // calculate rotation around y axis (euler angles are in the wrong order)
        auto yRot = Quaternion::Normalize({0, rot.y, 0, rot.w});
        // multiply to undo y axis rotation, inverse first to use global y axis
        rot = Quaternion::Inverse(yRot) * rot;
    }

    // before adding the new rotation to the average (mean), we have to check whether the quaternion has to be inverted
    // because q and -q are the same rotation, but cannot be averaged
    if (Quaternion::Dot(rot, baseRotation) < 0)
        rot = {-rot.x, -rot.y, -rot.z, -rot.w};

    cumulative.w += rot.w;
    cumulative.x += rot.x;
    cumulative.y += rot.y;
    cumulative.z += rot.z;

    num++;
}

Quaternion MetaCore::Engine::QuaternionAverage::GetAverage() const {
    // average values
    float avgMult = 1 / (float) num;
    float w = cumulative.w * avgMult;
    float x = cumulative.x * avgMult;
    float y = cumulative.y * avgMult;
    float z = cumulative.z * avgMult;

    // normalize
    float lengthD = 1 / (w * w + x * x + y * y + z * z);
    w *= lengthD;
    x *= lengthD;
    y *= lengthD;
    z *= lengthD;

    return Quaternion{x, y, z, w} * Quaternion::Inverse(baseRotation);
}
