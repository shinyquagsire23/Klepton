#pragma once

#include "../../_config.h"
#include "TypeHandler.hpp"
#include "UnityEngine/UI/Selectable.hpp"

namespace BSML {
    class BSML_EXPORT SelectableHandler : public TypeHandler<UnityEngine::UI::Selectable*> {
        public:
            using Base = TypeHandler<UnityEngine::UI::Selectable*>;
            SelectableHandler() : Base() {}

            virtual Base::PropMap get_props() const override;
            virtual Base::SetterMap get_setters() const override;
    };
}
