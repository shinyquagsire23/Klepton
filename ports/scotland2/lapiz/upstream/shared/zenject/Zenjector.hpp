#pragma once

#include "../_config.h"
#include "Location.hpp"
#include "System/Collections/Generic/IEnumerable_1.hpp"
#include "../concepts.hpp"
#include "../arrayutils.hpp"

#include "Zenject/DiContainer.hpp"
#include "Zenject/SceneDecoratorContext.hpp"

namespace Lapiz::Zenject {
    class ZenjectManager;

    namespace Internal {
        class InstallSet;
        class InstallInstruction;
        class MutateSet;
        class ExposeSet;
    }

    typedef std::function<void(::Zenject::DiContainer*)> ZenjectorCallback;

    template<typename T>
    requires(std::is_convertible_v<T, System::Object*>)
    using MutateCallback = std::function<void(::Zenject::SceneDecoratorContext*, T)>;

    class LAPIZ_EXPORT Zenjector {
        public:
            /// @brief Installs a custom installer to a location with a backing installer(s).
            /// @tparam T The type of your custom installer.
            /// @param location The location to install it to.
            /// @param parameters Parameters for the constructor of the installer. This will override Zenject's constructor injection on this installer,
            /// and the installer type cannot be a MonoInstaller if using this.
            template<Lapiz::concepts::IInstaller T>
            inline void Install(Zenject::Location location, ArrayW<System::Object*> parameters) {
                Install(classof(T), location, parameters);
            };

            /// @brief Installs a custom installer to a location with a backing installer(s).
            /// @tparam T The type of your custom installer.
            /// @tparam Targs The types of the arguments to pass to the object.
            /// @param location The location to install it to.
            /// @param parameters Parameters for the constructor of the installer. This will override Zenject's constructor injection on this installer,
            /// and the installer type cannot be a MonoInstaller if using this.
            template<Lapiz::concepts::IInstaller T, typename... Targs>
            inline void Install(Zenject::Location location, Targs&... parameters) {
                if constexpr (sizeof...(parameters) == 0) {
                    Install(classof(T), location, nullptr);
                } else {
                    Install(classof(T), location, ArrayUtils::box_array(parameters...));
                }
            };

            /// @brief Installs a custom installer to a location with a backing installer(s).
            /// @tparam TCustomInstaller The type of your custom installer.
            /// @tparam TBaseInstaller The installer to install TCustomInstaller with.
            /// @param parameters Parameters for the constructor of the installer. This will override Zenject's constructor injection on this installer,
            /// and the installer type cannot be a MonoInstaller if using this.
            template<Lapiz::concepts::IInstaller TCustomInstaller,  Lapiz::concepts::IInstaller TBaseInstaller>
            inline void Install(ArrayW<System::Object*> parameters) {
                Install(classof(TCustomInstaller), classof(TBaseInstaller), parameters);
            };

            /// @brief Installs a custom installer to a location with a backing installer(s).
            /// @tparam TCustomInstaller The type of your custom installer.
            /// @tparam TBaseInstaller The installer to install TCustomInstaller with.
            /// @tparam Targs The types of the arguments to pass to the object.
            /// @param parameters Parameters for the constructor of the installer. This will override Zenject's constructor injection on this installer,
            /// and the installer type cannot be a MonoInstaller if using this.
            template<Lapiz::concepts::IInstaller TCustomInstaller,  Lapiz::concepts::IInstaller TBaseInstaller, typename... Targs>
            inline void Install(Targs&... parameters) {
                if constexpr (sizeof...(parameters) == 0) {
                    Install(classof(TCustomInstaller), classof(TBaseInstaller), nullptr);
                } else {
                    Install(classof(TCustomInstaller), classof(TBaseInstaller), ArrayUtils::box_array(parameters...));
                }
            };

            /// @brief Install bindings to a custom location with a backing installer(s).
            /// @param location The location to install it to.
            /// @param installCallback The callback which is used to install custom bindings into the container.
            void Install(Zenject::Location location, ZenjectorCallback installCallback);

            /// @brief Install bindings alongsise another installer without a custom installer.
            /// @tparam TBaseInstaller The installer to install your bindings with.
            /// @param installCallback The callback which is used to install custom bindings into the container.
            template<Lapiz::concepts::IInstaller TBaseInstaller>
            void Install(ZenjectorCallback installCallback) {
                Install(classof(TBaseInstaller), installCallback);
            };

            /// @brief Searches a decorator context for the first instance that matches a type, then invokes a callback with that instance for it to be modified or mutated.
            /// @tparam T The type to mutate.
            /// @param contractName The contract name of the SceneDecoratorContext to search on.
            /// @param mutationCallback The callback used to mutate the object instance.
            template<typename T>
            requires(std::is_convertible_v<T, System::Object*> && !std::is_same_v<T, System::Object*>)
            void Mutate(std::string_view contractName, MutateCallback<T> mutationCallback) {
                Mutate(classof(T), std::string(contractName.data(), contractName.size()), [mutationCallback](::Zenject::SceneDecoratorContext* context, System::Object* obj){ mutationCallback(context, reinterpret_cast<T>(obj)); });
            }

            /// @brief Searches a decorator context for the first instance that matches a type, then invokes a callback with that instance for it to be modified or mutated.
            /// @param typeToMutate the Il2CppClass* of the type to mutate, this would be the class of the argument passed second in the mutationCallback
            /// @param contractName The contract name of the SceneDecoratorContext to search on.
            /// @param mutationCallback The callback used to mutate the object instance.
            void Mutate(Il2CppClass* typeToMutate, std::string contractName, MutateCallback<System::Object*> mutationCallback);

            /// @brief Searches a decorator context for the first instance that matches a type, then automatically binds them the the active container.
            /// @tparam T the type to expose
            /// @param contractName the contract name of the SceneDecoratorContext to search on
            template<typename T>
            void Expose(std::string_view contractName) {
                Expose(classof(T), std::string(contractName.data(), contractName.size()));
            }

            /// @brief Searches a decorator context for the first instance that matches a type, then automatically binds them the the active container.
            /// @param typeToExpose the type to expose
            /// @param contractName the contract name of the SceneDecoratorContext to search on
            void Expose(Il2CppClass* typeToExpose, std::string contractName);

            /// @brief get a zenjector for your mod
            /// @return A zenjector for you to install, expose or mutate things with
            static Zenjector* Get(const modloader::ModInfo& modInfo = {MOD_ID, VERSION, 0});

            /// @brief Install bindings to another installer without a custom installer
            static void UseMetadataBinder() {};

            /// @brief Lets you use Lapiz' HTTP service system. Not implemented, nor used currently.
            static void UseHttpService() {};

            /// @brief Makes it possible for Quest mods to sync to an external service, like GitHub. Not implemented, nor used currently.
            static void UseLapizSync() {};
        private:
            friend class ZenjectManager;
            explicit Zenjector(const modloader::ModInfo& modInfo);

            void Install(Il2CppClass* baseInstallerT, ZenjectorCallback installCallback);
            void Install(Il2CppClass* customInstallerT, Il2CppClass* baseInstallerT, ArrayW<System::Object*> parameters);
            void Install(Il2CppClass* customInstallerT, Zenject::Location location, ArrayW<System::Object*> parameters);

            modloader::ModInfo modInfo;

            /// @brief sets for an install, made with the Install method that takes a location
            std::unordered_set<Internal::InstallSet*> _installSets;
            /// @brief instructions for an install, made with the Install method that takes a baseInstaller
            std::unordered_set<Internal::InstallInstruction*> _installInstructions;
            /// @brief information for types to mutate
            std::unordered_set<Internal::MutateSet*> _mutateSets;
            /// @brief information for types to expose 😳
            std::unordered_set<Internal::ExposeSet*> _exposeSets;
    };
}
