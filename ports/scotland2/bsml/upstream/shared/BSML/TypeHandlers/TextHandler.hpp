#pragma once

#include "../../_config.h"
#include "TypeHandler.hpp"
#include "TMPro/TextMeshProUGUI.hpp"

namespace BSML {
    class BSML_EXPORT TextHandler : public TypeHandler<TMPro::TextMeshProUGUI*> {
        public:
            using Base = TypeHandler<TMPro::TextMeshProUGUI*>;
            TextHandler() : Base() {}

            virtual Base::PropMap get_props() const override;
            virtual Base::SetterMap get_setters() const override;
    };
}
