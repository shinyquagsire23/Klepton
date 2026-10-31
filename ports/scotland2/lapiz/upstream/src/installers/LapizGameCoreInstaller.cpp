#include "installers/LapizGameCoreInstaller.hpp"

#include "objects/ContractRecycler.hpp"

#include "Zenject/DiContainer.hpp"
#include "Zenject/ConcreteIdBinderGeneric_1.hpp"
#include "Zenject/FromBinderNonGeneric.hpp"

DEFINE_TYPE(Lapiz::Installers, LapizGameCoreInstaller);

namespace Lapiz::Installers {
    void LapizGameCoreInstaller::InstallBindings() {
        auto container = get_Container();

        /* Object API */
        container->BindInterfacesTo<::Lapiz::Objects::ContractRecycler*>()->AsSingle();
    }
}