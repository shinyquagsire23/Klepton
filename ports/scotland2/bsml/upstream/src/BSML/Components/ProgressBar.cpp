#include "BSML/Components/ProgressBar.hpp"
#include "Helpers/creation.hpp"
#include "Helpers/utilities.hpp"

#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Time.hpp"
#include "UnityEngine/Transform.hpp"
#include "UnityEngine/WaitForSecondsRealtime.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/RenderMode.hpp"
#include "UnityEngine/Texture2D.hpp"
#include "UnityEngine/Sprite.hpp"
#include "UnityEngine/SpriteMeshType.hpp"
#include "UnityEngine/Vector3.hpp"
#include "HMUI/CurvedCanvasSettings.hpp"

// type borrowed from https://github.com/darknight1050/QuestUI
DEFINE_TYPE(BSML, ProgressBar);

static inline UnityEngine::Vector3 operator/(UnityEngine::Vector3 vec, float v) {
    return {
        vec.x / v,
        vec.y / v,
        vec.z / v
    };
}
namespace BSML {
    void ProgressBar::OnDisable() {
        if (!inited) return;
        loadingBar->set_fillAmount(0);
    }

    void ProgressBar::Awake() {
        inited = false;
        showingMessage = false;
    }

    void ProgressBar::Update() {
        if (!canvas || !canvas->m_CachedPtr.m_value || !canvas->get_enabled()) return;
        float pong = UnityEngine::Time::get_time() * 0.35f;

        float graph = std::fmod(pong, 2);
        if (graph > 1) graph = (2 - graph);
        auto color = UnityEngine::Color::HSVToRGB(graph, 1, 1);
        loadingBar->set_color(color);
        headerText->set_color(color);
    }

    custom_types::Helpers::Coroutine ProgressBar::DisableCanvasRoutine(float time) {
        static constexpr const float increment = 0.2f;

        float accumulation = 0.0f;
        while (accumulation < time) {
            SetProgress(accumulation / time);
            co_yield reinterpret_cast<System::Collections::IEnumerator*>(UnityEngine::WaitForSecondsRealtime::New_ctor(increment));
            accumulation += increment;
        }

        get_gameObject()->SetActive(false);
        showingMessage = false;

        co_return;
    }

    void ProgressBar::ShowMessage(StringW text, float time) {
        ShowMessage(text);

        StopAllCoroutines();
        StartCoroutine(custom_types::Helpers::CoroutineHelper::New(DisableCanvasRoutine(time)));
    }

    void ProgressBar::ShowMessage(StringW text) {
        if (!inited) return;
        showingMessage = true;
        headerText->set_text(text);
        get_gameObject()->SetActive(true);
    }

    void ProgressBar::SetProgress(float progress) {
        if (!inited) return;
        loadingBar->set_fillAmount(std::clamp(progress, 0.0f, 1.0f));
        if (!get_gameObject()->get_active()) get_gameObject()->SetActive(true);
    }

    void ProgressBar::SetProgress(float progress, bool showBar) {
        if (!inited) return;
        loadingBar->set_fillAmount(std::clamp(progress, 0.0f, 1.0f));
        get_gameObject()->SetActive(showBar);
    }

    ProgressBar* ProgressBar::CreateProgressBar(UnityEngine::Vector3 position, UnityEngine::Vector3 scale, UnityEngine::Vector3 rotation, StringW mainText, StringW subText1, StringW subText2) {
        auto bar = UnityEngine::GameObject::New_ctor("LoadingStatus")->AddComponent<ProgressBar*>();
        auto barGameObject = bar->get_gameObject();
        auto barObjectTransform = bar->transform;
        barObjectTransform->set_position(position);
        barObjectTransform->set_eulerAngles(rotation);
        barObjectTransform->set_localScale(scale / 100.0f);

        bar->canvas = barGameObject->AddComponent<UnityEngine::Canvas*>();
        bar->canvas->set_renderMode(UnityEngine::RenderMode::WorldSpace);
        barGameObject->AddComponent<HMUI::CurvedCanvasSettings*>()->SetRadius(0.0f);

        auto ct = bar->canvas->transform;
        ct->set_position(position);
        ct->set_localScale(scale / 100);

        UnityEngine::Vector2 LoadingBarSize = {100, 10};
        UnityEngine::Color BackgroundColor = {0, 0, 0, 0.2f};
        auto rectTransform = ct.cast<UnityEngine::RectTransform>();
        rectTransform->set_sizeDelta({200, 50});

        // why set everything after creating it in the first place ?
        bar->subText2 = Helpers::CreateText(ct, subText2, {10, 31}, {100, 20});
        bar->subText2->set_fontSize(7.0f);
        bar->subText1 = Helpers::CreateText(ct, subText1, {10, 23}, {100, 20});
        bar->subText1->set_fontSize(9.0f);
        bar->headerText = Helpers::CreateText(ct, mainText,{10, 15}, {100, 20});
        bar->headerText->set_fontSize(15.0f);

        bar->loadingBackground = UnityEngine::GameObject::New_ctor("Background")->AddComponent<UnityEngine::UI::Image*>();
        rectTransform = bar->loadingBackground->transform.cast<UnityEngine::RectTransform>();
        rectTransform->SetParent(ct, false);
        rectTransform->set_sizeDelta(LoadingBarSize);
        bar->loadingBackground->set_color(BackgroundColor);

        bar->loadingBar = UnityEngine::GameObject::New_ctor("Loading Bar")->AddComponent<UnityEngine::UI::Image*>();
        rectTransform = bar->loadingBar->transform.cast<UnityEngine::RectTransform>();
        rectTransform->SetParent(ct, false);
        rectTransform->set_sizeDelta(LoadingBarSize);

        bar->loadingBar->set_sprite(BSML::Utilities::ImageResources::GetWhitePixel());
        bar->loadingBar->set_type(UnityEngine::UI::Image::Type::Filled);
        bar->loadingBar->set_fillMethod(UnityEngine::UI::Image::FillMethod::Horizontal);
        bar->loadingBar->set_color({1.0f, 1.0f, 1.0f, 0.5f});

        UnityEngine::Object::DontDestroyOnLoad(bar->get_gameObject());
        bar->inited = true;
        return bar;
    }
}
