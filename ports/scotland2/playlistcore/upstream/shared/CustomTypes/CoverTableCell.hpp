#pragma once

#include "CustomListSource.hpp"
#include "HMUI/HoverHint.hpp"
#include "HMUI/ImageView.hpp"

DECLARE_CLASS_CUSTOM(PlaylistCore, CoverTableCell, PlaylistCore::CustomTableCell) {
    DECLARE_INSTANCE_FIELD(HMUI::ImageView*, coverImage);
    DECLARE_INSTANCE_FIELD(HMUI::ImageView*, selectedImage);
    DECLARE_INSTANCE_FIELD(HMUI::HoverHint*, hoverHint);

    DECLARE_CTOR(ctor);
    void refreshVisuals();
    void init(UnityEngine::Sprite* sprite, std::string text);
    void setSprite(UnityEngine::Sprite* sprite);
    void setText(std::string text);
};
