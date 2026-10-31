#pragma once

#include "../../_config.h"
#include "TypeHandler.hpp"
#include "UnityEngine/UI/LayoutGroup.hpp"

namespace BSML {
    class BSML_EXPORT LayoutGroupHandler : public TypeHandler<UnityEngine::UI::LayoutGroup*> {
        public:
            using Base = TypeHandler<UnityEngine::UI::LayoutGroup*>;
            LayoutGroupHandler() : Base() {}

            virtual Base::PropMap get_props() const override;
            virtual Base::SetterMap get_setters() const override;

            virtual void HandleType(const ComponentTypeWithData& componentType, BSMLParserParams& parserParams) override;

    };
}
