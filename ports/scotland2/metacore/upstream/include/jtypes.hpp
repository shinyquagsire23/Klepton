#pragma once

#include <jni.h>

namespace MetaCore::Java {
    template <class T>
    struct TypeResolver {
        static constexpr auto JMethod = &JNIEnv::CallObjectMethodV;
        static constexpr auto JStaticMethod = &JNIEnv::CallStaticObjectMethodV;
        static constexpr auto JGetField = &JNIEnv::GetObjectField;
        static constexpr auto JGetStaticField = &JNIEnv::GetStaticObjectField;
        static constexpr auto JSetField = &JNIEnv::SetObjectField;
        static constexpr auto JSetStaticField = &JNIEnv::SetStaticObjectField;
    };

#define TYPE_RESOLUTION(type, jname) \
    template <> \
    struct TypeResolver<type> { \
        static constexpr auto JMethod = &JNIEnv::Call##jname##MethodV; \
        static constexpr auto JStaticMethod = &JNIEnv::CallStatic##jname##MethodV; \
        static constexpr auto JGetField = &JNIEnv::Get##jname##Field; \
        static constexpr auto JGetStaticField = &JNIEnv::GetStatic##jname##Field; \
        static constexpr auto JSetField = &JNIEnv::Set##jname##Field; \
        static constexpr auto JSetStaticField = &JNIEnv::SetStatic##jname##Field; \
    }

    TYPE_RESOLUTION(bool, Boolean);
    TYPE_RESOLUTION(uint8_t, Byte);
    TYPE_RESOLUTION(char, Byte);
    TYPE_RESOLUTION(uint16_t, Char);
    TYPE_RESOLUTION(short, Short);
    TYPE_RESOLUTION(int, Int);
    TYPE_RESOLUTION(long, Long);
    TYPE_RESOLUTION(float, Float);
    TYPE_RESOLUTION(double, Double);
    TYPE_RESOLUTION(jstring, Object);

    template <>
    struct TypeResolver<void> {
        static constexpr auto JMethod = &JNIEnv::CallVoidMethodV;
        static constexpr auto JStaticMethod = &JNIEnv::CallStaticVoidMethodV;
    };

#undef TYPE_RESOLUTION
}
