#pragma once

#include "../../_config.h"
#include "TypeHandler.hpp"
#include "UnityEngine/RectTransform.hpp"

namespace BSML {
    class BSML_EXPORT RectTransformHandler : public TypeHandler<UnityEngine::RectTransform*> {
        public:
            using Base = TypeHandler<UnityEngine::RectTransform*>;
            RectTransformHandler() : Base() {}

            virtual Base::PropMap get_props() const override;
            virtual Base::SetterMap get_setters() const override;
    };
}
