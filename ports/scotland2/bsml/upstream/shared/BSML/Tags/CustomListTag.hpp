#pragma once

#include "../../_config.h"
#include "BSMLTag.hpp"

namespace BSML {
    class BSML_EXPORT CustomListTag : public BSMLTag {
        public:
            CustomListTag() : BSMLTag() {}
            virtual void HandleChildren(UnityEngine::Transform* parent, BSMLParserParams& parserParams, std::vector<ComponentTypeWithData*>& componentInfo) const override;
        protected:
            virtual UnityEngine::GameObject* CreateObject(UnityEngine::Transform* parent) const override;
            virtual void parse(const tinyxml2::XMLElement& elem) override;

            std::string bsmlString;
    };
}
