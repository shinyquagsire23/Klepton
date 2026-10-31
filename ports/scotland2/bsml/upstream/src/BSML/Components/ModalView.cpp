#include "BSML/Components/ModalView.hpp"
#include "Helpers/delegates.hpp"

DEFINE_TYPE(BSML, ModalView);

namespace BSML {
    void ModalView::Awake() {
        add_blockerClickedEvent(MakeSystemAction(this, ___BlockerClicked_MethodRegistrator.info));
    }

    void ModalView::Show() {
        
        this->::HMUI::ModalView::Show(true, moveToCenter, nullptr);
    }

    void ModalView::Hide() {
        this->::HMUI::ModalView::Hide(true, nullptr);
    }

    void ModalView::BlockerClicked() {
        if (onHide) {
            onHide();
        }

        if (dismissOnBlockerClicked) {
            Hide();
        }
    }
}