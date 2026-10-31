#pragma once

#include "../ExposeSet.hpp"
#include "Zenject/Context.hpp"
#include "UnityEngine/MonoBehaviour.hpp"
#include <set>

namespace Lapiz::Zenject::Internal::Exposers {
    class ExposerManager {
        public:
            void Install(Internal::ExposeSet* exposeSet, ::Zenject::Context* ctx, std::set<UnityEngine::MonoBehaviour*> iterlist);
    };
}
