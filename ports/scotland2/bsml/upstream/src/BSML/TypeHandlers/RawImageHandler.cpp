#include "BSML/TypeHandlers/RawImageHandler.hpp"
#include "Helpers/utilities.hpp"

void SetImage(UnityEngine::UI::RawImage* image, const StringParseHelper& path);

namespace BSML {
    static RawImageHandler rawImageHandler{};

    RawImageHandler::Base::PropMap RawImageHandler::get_props() const {
        return {
            { "image", {"source", "src"}}
        };
    }

    RawImageHandler::Base::SetterMap RawImageHandler::get_setters() const {
        return {
            { "image", SetImage}
        };
    }
}

void SetImage(UnityEngine::UI::RawImage* image, const StringParseHelper& path) {
    if (path.size() > 1 && path[0] == '#') {
        // game icon
        image->set_texture(BSML::Utilities::FindTextureCached(path.substr(1)));
        if (!image->get_texture()) {
            ERROR("Could not find Texture with name {}", path.substr(1));
        }
    } else {
        // something else
        BSML::Utilities::GetData(path, [image](auto data){
            image->set_texture(BSML::Utilities::LoadTextureRaw(data));
        });
    }
}
