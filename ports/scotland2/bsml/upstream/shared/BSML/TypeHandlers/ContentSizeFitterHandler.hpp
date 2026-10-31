#pragma once

#include "../../_config.h"
#include "TypeHandler.hpp"
#include "UnityEngine/UI/ContentSizeFitter.hpp"

namespace BSML {
    class BSML_EXPORT ContentSizeFitterHandler : public TypeHandler<UnityEngine::UI::ContentSizeFitter*> {
        public:
            using Base = TypeHandler<UnityEngine::UI::ContentSizeFitter*>;
            ContentSizeFitterHandler() : Base() {}

            virtual Base::PropMap get_props() const override;
            virtual Base::SetterMap get_setters() const override;
    };
}
