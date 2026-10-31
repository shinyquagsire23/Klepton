#pragma once

#include "../../_config.h"
#include "TypeHandler.hpp"
#include "../Components/ScrollableContainer.hpp"

namespace BSML {
    class BSML_EXPORT ScrollableContainerHandler : public TypeHandler<BSML::ScrollableContainer*> {
        public:
            using Base = TypeHandler<BSML::ScrollableContainer*>;
            ScrollableContainerHandler() : Base() {}

            virtual Base::PropMap get_props() const override;
            virtual Base::SetterMap get_setters() const override;
            virtual void HandleType(const ComponentTypeWithData& componentType, BSMLParserParams& parserParams) override;
            virtual void HandleTypeAfterChildren(const ComponentTypeWithData& componentType, BSMLParserParams& parserParams) override;
            virtual void HandleTypeAfterParse(const ComponentTypeWithData& componentType, BSMLParserParams& parserParams) override;
    };
}
