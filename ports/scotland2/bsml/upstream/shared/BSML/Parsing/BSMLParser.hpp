#pragma once

#include "../../_config.h"
#include "../ComponentTypeWithData.hpp"
#include "BSMLParserParams.hpp"
#include "BSMLNode.hpp"

namespace BSML {
    class BSML_EXPORT BSMLParser {
        public:
            BSMLParser();
            ~BSMLParser();

            static std::shared_ptr<BSMLParser> parse(std::string_view str);
            static std::shared_ptr<BSMLParser> parse_and_construct(std::string_view str, UnityEngine::Transform* parent, System::Object* host);
            void Construct(UnityEngine::Transform* parent, System::Object* host);
            static std::shared_ptr<BSMLParserParams> Construct(const BSMLNode* root, UnityEngine::Transform* parent, System::Object* host);

            std::shared_ptr<BSMLParserParams> parserParams;
            BSMLNode* root;
    };
}
