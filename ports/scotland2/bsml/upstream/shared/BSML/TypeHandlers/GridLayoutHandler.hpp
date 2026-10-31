#pragma once

#include "../../_config.h"
#include "TypeHandler.hpp"
#include "UnityEngine/UI/GridLayoutGroup.hpp"

namespace BSML {
    class BSML_EXPORT GridLayoutHandler : public TypeHandler<UnityEngine::UI::GridLayoutGroup*> {
        public:
            using Base = TypeHandler<UnityEngine::UI::GridLayoutGroup*>;
            GridLayoutHandler() : Base() {}

            virtual Base::PropMap get_props() const override;
            virtual Base::SetterMap get_setters() const override;
    };
}
