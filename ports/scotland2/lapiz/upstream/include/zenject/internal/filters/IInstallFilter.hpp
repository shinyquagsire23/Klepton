#pragma once

#include "../ContextBinding.hpp"
namespace Lapiz::Zenject::Internal::Filters {
    class IInstallFilter {
        public:
            virtual bool ShouldInstall(ContextBinding* binding) const = 0;
    };
}
