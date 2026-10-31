#pragma once

#include <jni.h>

#include <string>

#include "export.h"

/* JNI field and method signatures
- Field signatures are simply their type in JNI form.
- Method signatures are of the form "(argument-types)return-type" with no spaces.
The JNI form of all types are as follows:
bool:             "Z"
byte or uint8_t:  "B"
char or uint16_t: "C"
short:            "S"
int:              "I"
long:             "J"
float:            "F"
double:           "D"
array:            "[element-type"
class:            "Lclass;"
For example, a method that takes a String and two floats and returns an int would have the signature:
"(Ljava/Lang/String;FF)I"
Notably class names in JNI are also different - instead of "." they use "/", or "$" for nested types.
*/

namespace MetaCore::Java {
    /// @brief A struct that holds information to find a java class
    struct FindClass {
        jclass clazz = nullptr;
        jobject instance = nullptr;
        std::string name = "";

        /// @brief Constructor with an already known java class
        /// @param clazz The already known java class to use
        FindClass(jclass clazz) : clazz(clazz) {}
        /// @brief Constructor with a java object
        /// @param instance The java object to use
        FindClass(jobject instance) : instance(instance) {}
        /// @brief Constructor with a known java class and a java object to use as non-static
        /// @param clazz The already known java class to use
        /// @param instance The java object to use
        FindClass(jclass clazz, jobject instance) : clazz(clazz), instance(instance) {}
        /// @brief Constructor with the name of a java class
        /// @param name The name of a java class
        FindClass(std::string name) : name(name) {}
    };

    /// @brief A struct that holds information to find a java method id
    struct FindMethodID {
        jmethodID method = nullptr;
        std::string name = "";
        std::string signature = "";

        /// @brief Constructor with an already known java method id
        /// @param method The already known java method id to use
        FindMethodID(jmethodID method) : method(method) {}
        /// @brief Constructor with the name and signature of a java method
        /// @param name The name of a java method
        /// @param signature The signature of a java method
        FindMethodID(std::string name, std::string signature) : name(name), signature(signature) {}
    };

    /// @brief A struct that holds information to find a java field id
    struct FindFieldID {
        jfieldID field = nullptr;
        std::string name = "";
        std::string signature = "";

        /// @brief Constructor with an already known java field id
        /// @param field The already known java field id to use
        FindFieldID(jfieldID field) : field(field) {}
        FindFieldID(std::string name, std::string signature) : name(name), signature(signature) {}
    };

    /// @brief Gets the java class for a FindClass
    /// @param env The JNIEnv instance for the thread
    /// @param clazz The information to find the class
    /// @return The java class if found, otherwise nullptr
    METACORE_EXPORT jclass GetClass(JNIEnv* env, FindClass clazz);
    /// @brief Gets the java method id for a FindMethodID
    /// @param env The JNIEnv instance for the thread
    /// @param clazz The information to find the class of the method
    /// @param method The information to find the method
    /// @return The java method id if found, otherwise nullptr
    METACORE_EXPORT jmethodID GetMethodID(JNIEnv* env, FindClass clazz, FindMethodID method);
    /// @brief Gets the java field id for a FindFieldID
    /// @param env The JNIEnv instance for the thread
    /// @param clazz The information to find the class of the field
    /// @param method The information to find the field
    /// @return The java field id if found, otherwise nullptr
    METACORE_EXPORT jfieldID GetFieldID(JNIEnv* env, FindClass clazz, FindFieldID field);
}
