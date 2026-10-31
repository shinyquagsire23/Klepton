#include "zenject/Zenjector.hpp"
#include "zenject/internal/InstallSet.hpp"
#include "zenject/internal/InstallInstruction.hpp"
#include "zenject/internal/filters/TypedInstallFilter.hpp"
#include "zenject/internal/filters/MultiTypedInstallFilter.hpp"
#include "zenject/internal/Delegate.hpp"
#include "zenject/internal/MutateSet.hpp"
#include "zenject/internal/ExposeSet.hpp"
#include "ZenjectManager.hpp"
#include "utilities/logging.hpp"

#include "AttributeRegistration_internal.hpp"

using namespace Lapiz::Zenject::Internal;
using namespace Lapiz::Zenject::Internal::Filters;

extern "C" void load();

namespace Lapiz::Zenject {
    Zenjector::Zenjector(const modloader::ModInfo& modInfo) : modInfo(modInfo) {
        ZenjectManager::get_instance().Add(this);
    }

    Zenjector* Zenjector::Get(const modloader::ModInfo& modInfo) {
        load();
        Lapiz::Attributes::AutoRegister();
        auto z = new Zenjector(modInfo);
        return z;
    }

    void Zenjector::Install(Zenject::Location location, ZenjectorCallback installCallback) {
        for (auto baseInstallerT : InstallerForLocation(location)) {
            _installInstructions.emplace(new InstallInstruction(baseInstallerT, installCallback));
        }
    }

    void Zenjector::Install(Il2CppClass* baseInstallerT, ZenjectorCallback installCallback) {
        _installInstructions.emplace(new InstallInstruction(baseInstallerT, installCallback));
    }

    void Zenjector::Install(Il2CppClass* customInstallerT, Il2CppClass* baseInstallerT, ArrayW<System::Object*> parameters) {
        auto filter = new TypedInstallFilter(baseInstallerT);
        _installSets.emplace(new InstallSet(customInstallerT, filter, (parameters && parameters.size() > 0) ? parameters : nullptr));
    }

    void Zenjector::Install(Il2CppClass* customInstallerT, Zenject::Location location, ArrayW<System::Object*> parameters) {
        auto filter = new MultiTypedInstallFilter(InstallerForLocation(location));
        _installSets.emplace(new InstallSet(customInstallerT, filter, (parameters && parameters.size() > 0) ? parameters : nullptr));
    }

    void Zenjector::Expose(Il2CppClass* typeToExpose, std::string contractName) {
        _exposeSets.emplace(new ExposeSet(typeToExpose, contractName));
    }

    void Zenjector::Mutate(Il2CppClass* typeToMutate, std::string contractName, std::function<void(::Zenject::SceneDecoratorContext*, System::Object*)> mutationCallback) {
        auto wrapper = new ZenjectDelegate();
        wrapper->Wrap(mutationCallback);
        _mutateSets.emplace(new MutateSet(typeToMutate, contractName, wrapper));
    }
}
