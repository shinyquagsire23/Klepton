#include "Modal.hpp"

#include <HMUI/ImageView.hpp>
#include <HMUI/NavigationController.hpp>
#include <UnityEngine/Canvas.hpp>
#include <UnityEngine/CanvasGroup.hpp>
#include <UnityEngine/GameObject.hpp>
#include <UnityEngine/Rect.hpp>
#include <UnityEngine/RectTransform.hpp>
#include <bsml/shared/BSML/MainThreadScheduler.hpp>
#include <bsml/shared/Helpers/delegates.hpp>
#include <UnityEngine/UI/LayoutRebuilder.hpp>

#include "Log.hpp"
#include "Utils.hpp"

namespace AirbudsSearch {

Modal::Modal(UnityW<HMUI::ModalView> modalView) : modalView_(modalView) {
}

void Modal::show() {
    // There is a bug where if you open a modal, then exit the view controller without closing the modal first, it
    // won't correctly dim the background the next time you open it. To fix this, we re-enable the canvas animations.
    modalView_->_animateParentCanvas = true;

    // For some reason, the default BSML modal doesn't correctly set the rect transform size, which results in pointer
    // hit testing not working. To fix this, we have to calculate the correct size once the modal is shown and layout
    // calculations have finished. Another quirk is that setting the size delta more than once seems to corrupt it, so
    // we have another check to only set it the first time the modal is shown.
    modalView_->Show(true, true, BSML::MakeSystemAction([this]() {
        if (isLayoutCalculated_) {
            return;
        }
        isLayoutCalculated_ = true;
        BSML::MainThreadScheduler::ScheduleNextFrame([this]() {
            // Find the maximum size delta of all the child transforms
            UnityEngine::Vector2 maxSizeDelta = {0, 0};
            for (size_t i = 0; i < modalView_->get_transform()->get_childCount(); ++i) {
                auto t = modalView_->get_transform()->GetChild(i)->GetComponent<UnityEngine::RectTransform*>();
                maxSizeDelta = {
                    std::max(maxSizeDelta.x, t->get_sizeDelta().x),
                    std::max(maxSizeDelta.y, t->get_sizeDelta().y)};
            }
            AirbudsSearch::Log.info("Calculated modal size: ({}, {:.2f})", maxSizeDelta.x, maxSizeDelta.y);

            // TODO: Resize if >70

            auto rectTransform = modalView_->GetComponent<UnityEngine::RectTransform*>();
            rectTransform->set_sizeDelta(maxSizeDelta);
        });
    }));
}

void Modal::hide(const bool animate) {
    auto onAnimationFinished = [this]() {
        modalView_->get_gameObject()->set_active(false);
    };

    modalView_->Hide(animate, BSML::MakeSystemAction([onAnimationFinished]() {
        onAnimationFinished();
    }));

    // The callback passed to modalView->Hide() doesn't get called if the animation is disabled, so we have to call it
    // manually.
    if (!animate) {
        onAnimationFinished();
    }
}

}// namespace AirbudsSearch
