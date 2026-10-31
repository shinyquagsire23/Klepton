#pragma once

#include "../../_config.h"
#include "TypeHandler.hpp"
#include "BSML/Components/ProgressBar.hpp"

namespace BSML {
    class BSML_EXPORT ProgressBarHandler : public TypeHandler<ProgressBar*> {
        public:
            using Base = TypeHandler<ProgressBar*>;
            ProgressBarHandler() : Base() {}

            virtual Base::PropMap get_props() const override;
            virtual Base::SetterMap get_setters() const override;
    };
}
