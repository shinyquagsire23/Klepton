#pragma once

#include "../../_config.h"
#include "TypeHandler.hpp"
#include "../Components/Backgroundable.hpp"

namespace BSML {
    class BSML_EXPORT BackgroundableHandler : public TypeHandler<Backgroundable*> {
        public:
            using Base = TypeHandler<Backgroundable*>;
            BackgroundableHandler() : Base() {}

            virtual Base::PropMap get_props() const override;
            virtual Base::SetterMap get_setters() const override;
    };
}
