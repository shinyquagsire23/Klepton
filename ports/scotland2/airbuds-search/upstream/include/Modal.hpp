#include <HMUI/ModalView.hpp>

#pragma once

namespace AirbudsSearch {

class Modal {
    public:

    explicit Modal(UnityW<HMUI::ModalView> modalView);

    void show();

    void hide(bool animate = true);

    private:

    UnityW<HMUI::ModalView> modalView_ = nullptr;

    bool isLayoutCalculated_ = false;
};

}
