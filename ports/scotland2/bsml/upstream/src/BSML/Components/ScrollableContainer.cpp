#include "BSML/Components/ScrollableContainer.hpp"
#include "Helpers/delegates.hpp"
#include "logging.hpp"

#include "HMUI/ButtonBinder.hpp"
#include "HMUI/VerticalScrollIndicator.hpp"
#include "HMUI/ItemForFocussedScrolling.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/Rect.hpp"
#include "UnityEngine/Time.hpp"
#include "UnityEngine/UI/Image.hpp"
#include "UnityEngine/UI/Button.hpp"
#include "UnityEngine/Vector2.hpp"

DEFINE_TYPE(BSML, ScrollableContainer);

using namespace UnityEngine;
using namespace UnityEngine::UI;

template<typename T>
T lerp(T a, T b, float t) {
    return a + (b - a) * t;
}

template<typename T>
T min(T a, T b) {
    return (a < b) * a + (a >= b) * b;
}

template<typename T>
T max(T a, T b) {
    return (a > b) * a + (a <= b) * b;
}

template<typename T>
T abs(T v) {
    return v * ((v > 0) - (v < 0));
}

namespace BSML {
    void ScrollableContainer::UpdateViewPortMask() {
        auto img = _viewport->GetComponent<Image*>();
        if (img) {
            img->set_enabled(get_maskOverflow());
        }
    }

    void ScrollableContainer::Awake() {
        _buttonBinder = HMUI::ButtonBinder::New_ctor();

        RefreshContent();
        RefreshBindings();
        runScrollAnim = false;
    }

    void ScrollableContainer::RefreshBindings() {
        if (!_buttonBinder) {
            ERROR("No button binder");
            return;
        }

        _buttonBinder->ClearBindings();
        if (_pageUpButton) {
            if (!upButtonAction)
                upButtonAction = MakeSystemAction(this, il2cpp_functions::class_get_method_from_name(klass, "PageUpButtonPressed", 0));
            _buttonBinder->AddBinding(_pageUpButton, upButtonAction);
        }
        if (_pageDownButton) {
            if (!downButtonAction)
                downButtonAction = MakeSystemAction(this, il2cpp_functions::class_get_method_from_name(klass, "PageDownButtonPressed", 0));
            _buttonBinder->AddBinding(_pageDownButton, downButtonAction);
        }
    }

    void ScrollableContainer::RefreshContent() {
        SetContentSize(_contentRectTransform->get_rect().get_height());
        contentHeight = _contentRectTransform->get_rect().get_height();
        RefreshBindings();
        ComputeScrollFocusPosY();
    }

    void ScrollableContainer::RefreshButtons() {
        if (_pageUpButton) {
            _pageUpButton->set_interactable(_destinationPos > 0);
        }
        if (_pageDownButton) {
            _pageDownButton->set_interactable(_destinationPos < contentHeight - (_viewport ? _viewport->get_rect().get_height() : 0));
        }
    }

    void ScrollableContainer::ContentSizeUpdated() {
        RefreshContent();
        RefreshButtons();
        this->ScrollTo(scrollToBottomOnUpdate ? std::numeric_limits<float>::max() : 0, false);
    }

    void ScrollableContainer::Update() {
        auto height = _contentRectTransform->get_rect().get_height();
        if (contentHeight != height && height > 0.0f)
            ContentSizeUpdated();

        if (runScrollAnim) {
            float num = lerp(_contentRectTransform->get_anchoredPosition().y, _destinationPos, Time::get_deltaTime() * _smooth);
            if (abs(num - _destinationPos) < 0.01f)
            {
                num = _destinationPos;
                runScrollAnim = false;
            }

            _contentRectTransform->set_anchoredPosition({0, num});
            UpdateVerticalScrollIndicator(num);
        }
    }

    void ScrollableContainer::ComputeScrollFocusPosY() {
        auto componentsInChildren = GetComponentsInChildren<HMUI::ItemForFocussedScrolling*>(true);
        _scrollFocusPositions = ArrayW<float>(il2cpp_array_size_t(componentsInChildren.size()));

        int i = 0;
        for (auto comp : componentsInChildren) {
            _scrollFocusPositions[i] = WorldPositionToScrollViewPosition(comp->get_transform()->get_position()).y;
            i++;
        }

        std::sort(_scrollFocusPositions->begin(), _scrollFocusPositions->end(), [](float a, float b) {
            return a < b;
        });
    }

    void ScrollableContainer::UpdateVerticalScrollIndicator(float posY) {
        if (_verticalScrollIndicator) {
            auto progress = posY / (contentHeight - _viewport->get_rect().get_height());
            _verticalScrollIndicator->set_progress(progress);
        }
    }

    void ScrollableContainer::ScrollDown(bool animated) {
        this->ScrollTo(get_maxPosition(), animated);
    }

    void ScrollableContainer::ScrollToWorldPosition(UnityEngine::Vector3 worldPosition, float pageRelativePosition, bool animated) {
        float num = WorldPositionToScrollViewPosition(worldPosition).y;
        num -= pageRelativePosition * get_scrollPageHeight();
        ScrollTo(num, animated);
    }

    void ScrollableContainer::ScrollToWorldPositionIfOutsideArea(UnityEngine::Vector3 worldPosition, float pageRelativePosition, float relativeBoundaryStart, float relativeBoundaryEnd, bool animated) {
        float num = WorldPositionToScrollViewPosition(worldPosition).y;
        float scrollPageHeight = get_scrollPageHeight();
        float num2 = _destinationPos + relativeBoundaryStart * scrollPageHeight;
        float num3 = _destinationPos + relativeBoundaryEnd * scrollPageHeight;
        if (num > num2 && num < num3)
        {
            return;
        }
        num -= pageRelativePosition * scrollPageHeight;
        ScrollTo(num, animated);
    }

    void ScrollableContainer::ScrollToNormalized(float dstPosY, bool animated) {
        ScrollTo(std::clamp(dstPosY, 0.0f, 1.0f) * get_maxPosition(), animated);
    }

    void ScrollableContainer::ScrollTo(float dstPosY, bool animated) {
        SetDestinationPosY(dstPosY);
        if (!animated)
            _contentRectTransform->set_anchoredPosition({0.0f, _destinationPos});
        RefreshButtons();
        runScrollAnim = true;
    }

    void ScrollableContainer::PageUpButtonPressed() {
        float num = _destinationPos;
        switch (_scrollType)
        {
            case ScrollType::PageSize:
                num -= _pageStepNormalizedSize * get_scrollPageHeight();
                break;
            case ScrollType::FixedCellSize:
                num -= _fixedCellSize * (float)(floor(get_scrollPageHeight() / _fixedCellSize) - 1);
                num = (float)floor(num / _fixedCellSize) * _fixedCellSize;
                break;
            case ScrollType::FocusItems:
                {
                    float threshold = _destinationPos + _scrollItemRelativeThresholdPosition * get_scrollPageHeight();

                    float max = _destinationPos;
                    for (auto posy : _scrollFocusPositions) {
                        if (posy >= threshold) continue;
                        if (posy > max) max = posy;
                    }
                    num = max;
                    num -= _pageStepNormalizedSize * get_scrollPageHeight();
                    break;
                }
        }

        ScrollTo(num, true);
        RefreshButtons();
        set_enabled(true);
    }

    void ScrollableContainer::PageDownButtonPressed() {
        float num = _destinationPos;
        switch (_scrollType)
        {
            case ScrollType::PageSize:
                num += _pageStepNormalizedSize * get_scrollPageHeight();
                break;
            case ScrollType::FixedCellSize:
                num += _fixedCellSize * (float)(ceil(get_scrollPageHeight() / _fixedCellSize) - 1);
                num = (float)ceil(num / _fixedCellSize) * _fixedCellSize;
                break;
            case ScrollType::FocusItems:
                {
                    float threshold = _destinationPos + (1.0f - _scrollItemRelativeThresholdPosition) * get_scrollPageHeight();
                    float min = _destinationPos + get_scrollPageHeight();
                    for (auto posy : _scrollFocusPositions) {
                        if (posy <= threshold) continue;
                        if (posy < min) min = posy;
                    }
                    num = min;
                    num -= (1.0f - _pageStepNormalizedSize) * get_scrollPageHeight();
                    break;
                }
        }

        ScrollTo(num, true);
        RefreshButtons();
        set_enabled(true);
    }

    void ScrollableContainer::SetDestinationPosY(float value) {
        float maxPosition = get_maxPosition();
        if (maxPosition < 0 && !alignBottom) maxPosition = 0.0f;
        _destinationPos = min(maxPosition, max(0.0f, value));
    }

    bool ScrollableContainer::get_alignBottom() {
        return alignBottom;
    }

    void ScrollableContainer::set_alignBottom(bool value) {
        alignBottom = value;
        this->ScrollTo(_destinationPos, true);
    }

    bool ScrollableContainer::get_maskOverflow() {
        return maskOverflow;
    }

    void ScrollableContainer::set_maskOverflow(bool value) {
        maskOverflow = value;
        UpdateViewPortMask();
    }

    float ScrollableContainer::get_scrollPageHeight() {
        return _viewport ? _viewport->get_rect().get_height() : 0.0f;
    }

    float ScrollableContainer::get_maxPosition() {
        return contentHeight - _viewport->get_rect().get_height();
    }
}
