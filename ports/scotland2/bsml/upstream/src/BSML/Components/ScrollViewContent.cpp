#include "BSML/Components/ScrollViewContent.hpp"
#include "logging.hpp"

#include "HMUI/ScrollView.hpp"

#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/MonoBehaviour.hpp"
#include "UnityEngine/Vector2.hpp"
#include "UnityEngine/Rect.hpp"

#include "UnityEngine/UI/LayoutRebuilder.hpp"

DEFINE_TYPE(BSML, ScrollViewContent);

namespace BSML {
    void ScrollViewContent::Start() {
        UnityEngine::UI::LayoutRebuilder::ForceRebuildLayoutImmediate(transform.cast<UnityEngine::RectTransform>());
        dirty = true;
    }

    void ScrollViewContent::OnEnable() {
        UpdateScrollView();
    }

    void ScrollViewContent::OnRectTransformDimensionsChange() {
        dirty = true;
    }

    void ScrollViewContent::Update() {
        if (dirty) {
            dirty = false;
            UpdateScrollView();
        }
    }

    void ScrollViewContent::UpdateScrollView() {
        if (!scrollView || !scrollView->m_CachedPtr.m_value) {
            return;
        }

        scrollView->SetContentSize(Content->rect.height);
        scrollView->RefreshButtons();
    }

    UnityEngine::RectTransform* ScrollViewContent::get_Content() {
        if (content && content->m_CachedPtr.m_value) return content;
        content = transform->GetChild(0).cast<UnityEngine::RectTransform>();
        return content;
    }

    UnityEngine::RectTransform* ScrollViewContent::get_rectTransform() {
        return transform.try_cast<UnityEngine::RectTransform>().value_or(nullptr);
    }
}
