#pragma once

#include "../MutateSet.hpp"
#include "UnityEngine/MonoBehaviour.hpp"


namespace Lapiz::Zenject::Internal::Mutators {
    class MutatorManager {
        public:
            void Install(Internal::MutateSet* mutateSet, ::Zenject::Context* ctx, std::set<UnityEngine::MonoBehaviour*> iterlist);
    };
}
