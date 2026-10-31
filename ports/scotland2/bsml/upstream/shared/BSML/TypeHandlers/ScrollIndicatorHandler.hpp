#pragma once

#include "../../_config.h"
#include "../Components/ScrollIndicator.hpp"
#include "TypeHandler.hpp"

namespace BSML {
    class BSML_EXPORT ScrollIndicatorHandler : public TypeHandler<BSML::ScrollIndicator*> {
        public:
            using Base = TypeHandler<BSML::ScrollIndicator*>;
            ScrollIndicatorHandler() : Base() {}

            virtual Base::PropMap get_props() const override;
            virtual Base::SetterMap get_setters() const override;
    };
}
