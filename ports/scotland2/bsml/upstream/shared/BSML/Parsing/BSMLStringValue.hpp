#pragma once

#include "../../_config.h"
#include "BSMLValue.hpp"

namespace BSML {
    struct BSML_EXPORT BSMLStringValue : public BSMLValue {
        std::string value;

        void SetValue(System::Object* val) override;
        System::Object* GetValue() override;
    };
}
