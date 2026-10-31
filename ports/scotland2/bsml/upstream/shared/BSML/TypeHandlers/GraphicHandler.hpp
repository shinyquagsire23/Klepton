#pragma once

#include "../../_config.h"
#include "TypeHandler.hpp"
#include "UnityEngine/UI/Graphic.hpp"

namespace BSML {
    class BSML_EXPORT GraphicHandler : public TypeHandler<UnityEngine::UI::Graphic*> {
        public:
            using Base = TypeHandler<UnityEngine::UI::Graphic*>;
            GraphicHandler() : Base() {}

            virtual Base::PropMap get_props() const override;
            virtual Base::SetterMap get_setters() const override;
    };
}
