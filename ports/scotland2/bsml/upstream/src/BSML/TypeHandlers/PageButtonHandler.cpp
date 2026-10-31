#include "BSML/TypeHandlers/PageButtonHandler.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/Vector2.hpp"
#include "UnityEngine/Quaternion.hpp"
#include "UnityEngine/UI/LayoutElement.hpp"

enum class PageButtonDirection {
    Up,
    Down,
    Left,
    Right
};

static std::map<std::string, PageButtonDirection> stringToPageButtonDirectionMap {
    {"Up", PageButtonDirection::Up},
    {"Down", PageButtonDirection::Down},
    {"Left", PageButtonDirection::Left},
    {"Right", PageButtonDirection::Right}
};

static PageButtonDirection stringToPageButtonDirection(const std::string& str) {
    auto itr = stringToPageButtonDirectionMap.find(str);
    if (itr == stringToPageButtonDirectionMap.end()) return PageButtonDirection::Up;
    return itr->second;
}

void SetButtonDirection(BSML::PageButton* pageButton, const StringParseHelper& value);

namespace BSML {
    static PageButtonHandler pageButtonHandler{};

    PageButtonHandler::Base::PropMap PageButtonHandler::get_props() const {
        return {
            {"direction", {"dir", "direction"}}
        };
    }

    PageButtonHandler::Base::SetterMap PageButtonHandler::get_setters() const {
        return {
            {"direction", SetButtonDirection}
        };
    }
}

void SetButtonDirection(BSML::PageButton* pageButton, const StringParseHelper& value) {
        bool isHorizontal = false;
        int angle = 0;
        auto pageButtonDirection = stringToPageButtonDirection(value);
        auto buttonTransform = pageButton->transform->Find("Icon").cast<UnityEngine::RectTransform>();
        buttonTransform->set_anchoredPosition({0, 0});
        auto layoutElement = pageButton->GetComponent<UnityEngine::UI::LayoutElement*>();
        switch(pageButtonDirection) {
            case PageButtonDirection::Up:
                isHorizontal = true;
                angle = -180;
                break;
            case PageButtonDirection::Down:
                isHorizontal = true;
                angle = 0;
                break;
            case PageButtonDirection::Left:
                isHorizontal = false;
                angle = -90;
                break;
            case PageButtonDirection::Right:
                isHorizontal = false;
                angle = 90;
                break;
        }
        buttonTransform->set_localRotation(UnityEngine::Quaternion::Euler(0, 0, angle));
        if (layoutElement->get_preferredHeight() == -1) {
            layoutElement->set_preferredHeight(isHorizontal ? 6 : 40);
        }

        if (layoutElement->get_preferredWidth() == -1) {
            layoutElement->set_preferredWidth(isHorizontal ? 40 : 6);
        }
}
