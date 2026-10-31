#pragma once
#include <list>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "logging.hpp"
#include "types.hpp"
#include "./_config.h"
#include "beatsaber-hook/shared/utils/typedefs.h"

template <>
struct std::hash<std::pair<std::string, std::string>> {
    size_t operator()(const std::pair<std::string, std::string>& item) const noexcept {
        std::size_t h1 = std::hash<std::string>{}(item.first);
        std::size_t h2 = std::hash<std::string>{}(item.second);
        return h1 ^ (h2 << 1);
    }
};

namespace custom_types {
/// @class Public API for registering types
class Register {
    friend TypeRegistration;

   private:
    CUSTOM_TYPES_EXPORT static std::unordered_map<std::string, Il2CppAssembly*> assembs;
    CUSTOM_TYPES_EXPORT static std::unordered_map<std::string, Il2CppImage*> images;
    CUSTOM_TYPES_EXPORT static std::shared_mutex assemblyMtx;
    CUSTOM_TYPES_EXPORT static std::shared_mutex imageMtx;
    CUSTOM_TYPES_EXPORT static std::mutex registrationMtx;
    CUSTOM_TYPES_EXPORT static std::mutex classMappingMtx;
    CUSTOM_TYPES_EXPORT static bool installed;

    CUSTOM_TYPES_EXPORT static std::vector<TypeRegistration*> toRegister;
    CUSTOM_TYPES_EXPORT static std::vector<TypeRegistration*> registeredTypes;
    CUSTOM_TYPES_EXPORT static TypeDefinitionIndex typeIdx;

    CUSTOM_TYPES_EXPORT static Il2CppAssembly* createAssembly(std::string_view name, Il2CppImage* img);
    CUSTOM_TYPES_EXPORT static const Il2CppImage* createImage(std::string_view name);

    CUSTOM_TYPES_EXPORT static void EnsureHooks();

    static void addToMapping(const TypeRegistration* itr) {
        auto namePair = std::make_pair(std::string(itr->namespaze()), std::string(itr->name()));
        if (!classMapping.contains(namePair)) {
            custom_types::logger.debug("Adding new type to class mapping: {}", fmt::ptr(itr->klass()));
            // Only add to the class mapping if the exact type created does not
            // already exist.
            classMapping.insert({ namePair, itr->klass() });
        }
    }

   public:
    CUSTOM_TYPES_EXPORT static std::unordered_map<std::pair<std::string, std::string>, Il2CppClass*> classMapping;
    CUSTOM_TYPES_EXPORT static std::vector<Il2CppClass*> classes;
    CUSTOM_TYPES_EXPORT static std::vector<TypeRegistration*> const& getTypes() {
        return registeredTypes;
    }
    /// @brief Automatically registers all pending types.
    /// To add a type to be registered, see: AddType
    /// To get the list of all registered types, see: getTypes()
    CUSTOM_TYPES_EXPORT static void AutoRegister() {
        EnsureHooks();
        std::lock_guard<std::mutex> lck(registrationMtx);

        {
            std::lock_guard lock(classMappingMtx);
            for (auto itr : toRegister) {
                itr->createClass();
                custom_types::logger.debug(
                    "Created class! registration: {}, {}::{} klass: {}, "
                    "{}::{}, image: {}",
                    fmt::ptr(itr), itr->namespaze(), itr->name(), fmt::ptr(itr->klass()), itr->klass()->namespaze, itr->klass()->name, fmt::ptr(itr->klass()->image));
                addToMapping(itr);
            }
        }
        for (auto actual : toRegister) {
            // Populate fields, methods, etc. here
            if (!actual->initialized()) {
                actual->populateFields();
                actual->populateMethods();
                actual->setInitialized();
            }
            custom_types::logger.debug("Registered registration: {}, {}::{} klass: {}, {}::{}, image: {}", fmt::ptr(actual), actual->namespaze(), actual->name(), fmt::ptr(actual->klass()), actual->klass()->namespaze,
                            actual->klass()->name, fmt::ptr(actual->klass()->image));
            registeredTypes.push_back(actual);
        }
        toRegister.clear();
    }

    CUSTOM_TYPES_EXPORT static void ExplicitRegister(std::unordered_set<TypeRegistration*> toAdd) {
        EnsureHooks();
        std::lock_guard<std::mutex> lck(registrationMtx);

        // Remove everything we are explicitly registering from the collection of
        // types that would be auto registered. This allows us to avoid duplicate
        // registrations.
        auto itr = toRegister.begin();
        while (itr != toRegister.end()) {
            if (toAdd.contains(*itr)) {
                itr = toRegister.erase(itr);
            } else {
                ++itr;
            }
        }
        {
            std::lock_guard lock(classMappingMtx);
            for (auto itr : toAdd) {
                itr->createClass();
                custom_types::logger.debug(
                    "Created class! registration: {}, {}::{} klass: {}, "
                    "{}::{}, image: {}",
                    fmt::ptr(itr), itr->namespaze(), itr->name(), fmt::ptr(itr->klass()), itr->klass()->namespaze, itr->klass()->name, fmt::ptr(itr->klass()->image));
                addToMapping(itr);
            }
        }
        for (auto actual : toAdd) {
            if (!actual->initialized()) {
                actual->populateFields();
                actual->populateMethods();
                actual->setInitialized();
            }
            custom_types::logger.debug("Registered registration: {}, {}::{} klass: {}, {}::{}, image: {}", fmt::ptr(actual), actual->namespaze(), actual->name(), fmt::ptr(actual->klass()), actual->klass()->namespaze,
                            actual->klass()->name, fmt::ptr(actual->klass()->image));
            registeredTypes.push_back(actual);
        }
    }

    CUSTOM_TYPES_EXPORT static void AddType(TypeRegistration* type) {
        custom_types::logger.debug("Added instance to register: {}", fmt::ptr(type));
        toRegister.push_back(type);
    }

    /// @brief Unregisters all custom types, deleting all associated data.
    /// You should not (realistically) have a need for this unless you find custom
    /// types memory overhead to be significant.
    CUSTOM_TYPES_EXPORT static void UnregisterAll() {
        std::lock_guard lck(registrationMtx);
        Register::typeIdx = kTypeDefinitionIndexInvalid;
        for (auto itr : registeredTypes) {
            itr->clear();
        }
        registeredTypes.clear();
        // This will obviously cause all sorts of issues if these types are
        // currently being used within il2cpp.
        classes.clear();
        classMapping.clear();
        // We also need to correctly delete any created images or assemblies.
        // These need to deleted in a very delicate way so as not to have any
        // dangling references.
    }
};
}  // namespace custom_types
