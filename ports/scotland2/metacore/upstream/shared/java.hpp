#pragma once

#include "export.h"
#include "jutils.hpp"

namespace MetaCore::Java {
    /// @brief A struct to automatically create and clean up a local JNI frame
    struct JNIFrame {
        /// @brief Constructor that allocates a frame of a given size
        /// @param env The JNIEnv instance for the thread
        /// @param size The amount of space to reserve on the frame
        JNIFrame(JNIEnv* env, int size = 0) : env(env) { env->PushLocalFrame(size); }
        /// @brief Destructor that frees the allocated frame if not already freed
        ~JNIFrame() { pop(); }

        /// @brief Frees the allocated frame if not already freed
        void pop() {
            if (env)
                env->PopLocalFrame(nullptr);
            env = nullptr;
        }

       private:
        JNIEnv* env;
    };

    /// @brief Gets a JNIEnv instance for the current thread - should be called as little as possible
    /// @return The JNIEnv instance for the thread
    METACORE_EXPORT JNIEnv* GetEnv();

    /// @brief Creates a new instance of the provided java class
    /// @param env The JNIEnv instance for the thread
    /// @param clazz The information to find the class of the method
    /// @param init The information to find the class' constructor method
    /// @param ... The arguments for the constructor
    /// @return The created java class instance, or nullptr if it failed
    METACORE_EXPORT jobject NewObject(JNIEnv* env, FindClass clazz, FindMethodID init, ...);
    /// @brief Creates a new instance of the provided java class
    /// @param env The JNIEnv instance for the thread
    /// @param clazz The information to find the class of the method
    /// @param init The signature of the class constructor method (will return void)
    /// @param ... The arguments for the constructor
    /// @return The created java class instance, or nullptr if it failed
    METACORE_EXPORT jobject NewObject(JNIEnv* env, FindClass clazz, std::string init, ...);

    /// @brief Runs a static or instance method on a java class or object
    /// @tparam T The c++ return type of the method
    /// @param env The JNIEnv instance for the thread
    /// @param clazz The information to find the class of the method, or the instance if non-static
    /// @param method The information to find the method
    /// @param ... The arguments for the method
    /// @return The return value of the method
    template <class T = void>
    METACORE_EXPORT T RunMethod(JNIEnv* env, FindClass clazz, FindMethodID method, ...);

    /// @brief Gets the value of a static or instance field on a java class or object
    /// @tparam T The c++ type of the field
    /// @param env The JNIEnv instance for the thread
    /// @param clazz The information to find the class of the field, or the instance if non-static
    /// @param field The information to find the field
    /// @return The value of the field
    template <class T>
    METACORE_EXPORT T GetField(JNIEnv* env, FindClass clazz, FindFieldID field);

    /// @brief Sets the value of a static or instance field on a java class or object
    /// @tparam T The c++ type of the field
    /// @param env The JNIEnv instance for the thread
    /// @param clazz The information to find the class of the field, or the instance if non-static
    /// @param field The information to find the field
    /// @param value The new value for the field
    template <class T>
    METACORE_EXPORT void SetField(JNIEnv* env, FindClass clazz, FindFieldID field, T value);

    /// @brief Loads a new java class from its compiled bytecode (see the Hollywood mod for a build setup)
    /// @param env The JNIEnv instance for the thread
    /// @param dexBytes The compiled bytes of the class
    /// @return A global ref to the newly loaded class
    METACORE_EXPORT jclass LoadClass(JNIEnv* env, std::string name, std::string_view dexBytes);

    /// @brief Gets the c++ string version of a java string object
    /// @param env The JNIEnv instance for the thread
    /// @param string The java string object
    /// @return The c++ version of the string
    METACORE_EXPORT std::string ConvertString(JNIEnv* env, jstring string);

    /// @brief Returns the result of the java toString method on the object
    /// @param env The JNIEnv instance for the thread
    /// @param string The java object
    /// @return The c++ string representation of the object
    METACORE_EXPORT std::string ToString(JNIEnv* env, jobject object);

    /// @brief Gets the name of a java class
    /// @param env The JNIEnv instance for the thread
    /// @param clazz The java class
    /// @return The java class name
    METACORE_EXPORT std::string GetClassName(JNIEnv* env, jclass clazz);

    /// @brief Gets the class, error message, and stack trace of a java exception
    /// @param env The JNIEnv instance for the thread
    /// @param error The java exception, or throwable of any kind
    /// @return The description of the exception
    METACORE_EXPORT std::string DescribeError(JNIEnv* env, jthrowable error);

#define SPECIALIZATION(type)                                                                                \
    extern template METACORE_EXPORT type RunMethod(JNIEnv* env, FindClass clazz, FindMethodID method, ...); \
    extern template METACORE_EXPORT type GetField(JNIEnv* env, FindClass clazz, FindFieldID field);         \
    extern template METACORE_EXPORT void SetField(JNIEnv* env, FindClass clazz, FindFieldID field, type value);

    SPECIALIZATION(jobject);
    SPECIALIZATION(jstring);
    SPECIALIZATION(bool);
    SPECIALIZATION(uint8_t);
    SPECIALIZATION(char);
    SPECIALIZATION(short);
    SPECIALIZATION(int);
    SPECIALIZATION(long);
    SPECIALIZATION(float);
    SPECIALIZATION(double);

    extern template METACORE_EXPORT void RunMethod(JNIEnv* env, FindClass clazz, FindMethodID method, ...);

#undef SPECIALIZATION
}
