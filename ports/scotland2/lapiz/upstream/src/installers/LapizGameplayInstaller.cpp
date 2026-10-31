#include "installers/LapizGameplayInstaller.hpp"

#include "sabers/SaberModelProvider.hpp"
#include "sabers/SaberModelManager.hpp"
#include "sabers/LapizSaberFactory.hpp"

#include "sabers/effects/ObstacleSaberSparkleEffectManagerLatch.hpp"
#include "sabers/effects/SaberBurnMarkSparklesLatch.hpp"
#include "sabers/effects/SaberClashEffectAdjuster.hpp"
#include "sabers/effects/SaberBurnMarkAreaLatch.hpp"


#include "Zenject/DiContainer.hpp"
#include "Zenject/ConcreteIdBinderGeneric_1.hpp"
#include "Zenject/FromBinderNonGeneric.hpp"

DEFINE_TYPE(Lapiz::Installers, LapizGameplayInstaller);

namespace Lapiz::Installers {
    void LapizGameplayInstaller::InstallBindings() {
        auto container = get_Container();

        /* Saber API */
        container->BindInterfacesTo<Lapiz::Sabers::Effects::ObstacleSaberSparkleEffectManagerLatch*>()->AsSingle();
        container->BindInterfacesTo<Lapiz::Sabers::Effects::SaberBurnMarkSparklesLatch*>()->AsSingle();
        container->BindInterfacesAndSelfTo<Lapiz::Sabers::SaberModelProvider*>()->AsSingle();
        container->BindInterfacesAndSelfTo<Lapiz::Sabers::SaberModelManager*>()->AsSingle();
        container->BindInterfacesTo<Lapiz::Sabers::Effects::SaberClashEffectAdjuster*>()->AsSingle();
        container->BindInterfacesTo<Lapiz::Sabers::Effects::SaberBurnMarkAreaLatch*>()->AsSingle();
        container->Bind<Lapiz::Sabers::LapizSaberFactory*>()->AsSingle();
    }
}