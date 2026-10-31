#pragma once

#include "../../_config.h"
#include "TypeHandler.hpp"
#include "../Components/ClickableText.hpp"

namespace BSML {
    class BSML_EXPORT ClickableTextHandler : public TypeHandler<BSML::ClickableText*> {
        public:
            using Base = TypeHandler<BSML::ClickableText*>;
            ClickableTextHandler() : Base() {}

            virtual Base::PropMap get_props() const override;
            virtual Base::SetterMap get_setters() const override;

            virtual void HandleTypeAfterParse(const ComponentTypeWithData& componentType, BSMLParserParams& parserParams) override;
    };
}
