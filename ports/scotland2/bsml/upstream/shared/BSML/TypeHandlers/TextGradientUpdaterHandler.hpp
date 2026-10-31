#pragma once

#include "../../_config.h"
#include "TypeHandler.hpp"
#include "../Components/TextGradientUpdater.hpp"

namespace BSML {
    class BSML_EXPORT TextGradientUpdaterHandler : public TypeHandler<BSML::TextGradientUpdater*> {
        public:
            using Base = TypeHandler<BSML::TextGradientUpdater*>;
            TextGradientUpdaterHandler() : Base() {}

            virtual Base::PropMap get_props() const override;
            virtual Base::SetterMap get_setters() const override;
    };
}
