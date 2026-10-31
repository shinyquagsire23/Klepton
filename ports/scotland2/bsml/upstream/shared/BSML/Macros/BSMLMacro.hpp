#pragma once

#include "../../_config.h"
#include "../Parsing/BSMLNode.hpp"
#include <map>

namespace BSML {
    class BSML_EXPORT BSMLMacro : public BSMLNode {
        public:
            BSMLMacro();
            using PropMap = std::map<std::string, std::vector<std::string>>;

            const PropMap& get_cachedProps() const;
            virtual PropMap get_props() const = 0;

            virtual void Handle(UnityEngine::Transform* parent, BSMLParserParams& parserParams, std::vector<ComponentTypeWithData*>& componentInfo) const override;
            virtual void Execute(UnityEngine::Transform* parent, const std::map<std::string, std::string>& data, BSMLParserParams& parserParams,  std::vector<ComponentTypeWithData*>& componentInfo) const = 0;
        private:
            PropMap cachedProps;
    };
}
