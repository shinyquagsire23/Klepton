#include "objects/ContractRecycler.hpp"
#include "objects/RedecoratorRegistration.hpp"

DEFINE_TYPE(Lapiz::Objects, ContractRecycler);

namespace Lapiz::Objects {
    void ContractRecycler::ctor(::Zenject::DiContainer* container) {
        _container = container->AncestorContainers->FirstOrDefault();
    }
    void ContractRecycler::Dispose() {
        while (_container->Unbind<::Lapiz::Objects::RedecoratorRegistration*>()) {}
    }
}
