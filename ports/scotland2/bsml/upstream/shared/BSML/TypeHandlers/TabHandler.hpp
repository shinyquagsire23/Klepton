#pragma once

#include "../../_config.h"
#include "TypeHandler.hpp"
#include "../Components/Tab.hpp"

namespace BSML {
    class BSML_EXPORT TabHandler : public TypeHandler<BSML::Tab*> {
        public:
            using Base = TypeHandler<BSML::Tab*>;
            TabHandler() : Base() {}

            virtual Base::PropMap get_props() const override;
            virtual Base::SetterMap get_setters() const override;
    };
}
