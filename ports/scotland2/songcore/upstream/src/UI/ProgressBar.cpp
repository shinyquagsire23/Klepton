#include "UI/ProgressBar.hpp"
#include "SongLoader/CustomBeatmapLevel.hpp"
#include "SongLoader/RuntimeSongLoader.hpp"
#include "bsml/shared/BSML-Lite.hpp"
#include "UnityEngine/Texture2D.hpp"
#include "logging.hpp"
#include "custom-types/shared/coroutine.hpp"
#include "bsml/shared/BSML/SharedCoroutineStarter.hpp"
#include <cstdio>
#include <string>
#include "UnityEngine/Time.hpp"

DEFINE_TYPE(SongCore::UI, ProgressBar);

namespace SongCore::UI {
    /// @brief method to get a random gradient to use
    BSML::Gradient* GetGradient(bool alwaysUseFlags = false) {
        auto rawTime = time(nullptr);
        auto localTime = localtime(&rawTime);

        // month is 0 index (jan == 0, feb == 1, ...)
        if (alwaysUseFlags || localTime->tm_mon == 5) {
            static BSML::Gradient* gradients[] = {
                BSML::Gradient::Parse("#e40303;#ff8c00;#ffed00;#008026;#004dff;#450787;#e40303"),
                BSML::Gradient::Parse("#59c9f4;#f1a5b5;#ffffff;#f1a5b5;#59c9f4"),
                BSML::Gradient::Parse("#d52c01;#fd9a57;#ffffff;#d1619f;#a20161;#d52c01"),
                BSML::Gradient::Parse("#078e70;#26ceaa;#98e8c1;#ffffff;#7bade2;#5049cb;#3d1a78;#078e70"),
                BSML::Gradient::Parse("#d0036d;#d0036d;#9a4f96;#0037a3;#0037a3;#d0036d"),
                BSML::Gradient::Parse("#000000;#808080;#ffffff;#800080;#000000"),
                BSML::Gradient::Parse("#3da542;#a8d379;#ffffff;#a9a9a9;#000000;#3da542"),
                BSML::Gradient::Parse("#f11a85;#f11a85;#ffd900;#ffd900;#1bb3ff;#1bb3ff;#f11a85"),
                BSML::Gradient::Parse("#f9ee33;#000000;#9c59cf;#2d2d2d;#f9ee33"),
                BSML::Gradient::Parse("#ac77d0;#ac77d0;#ffffff;#ffffff;#4b8124;#4b8124;#ac77d0"),
                BSML::Gradient::Parse("#ff75a2;#f5f5f5;#be18d6;#2c2c2c;#333ebd;#ff75a2"),
                BSML::Gradient::Parse("#000000;#bcc4c6;#ffffff;#b9f480;#ffffff;#bcc4c6;#000000")
            };

            auto idx = rand() % (sizeof(gradients) / sizeof(void*));
            return gradients[idx];
        } else {
            static auto gradient = BSML::Gradient::Parse("#ff6060;#ffa060;#ffff60;#a0ff60;#60ff60;#60ffa0;#60ffff;#60a0ff;#6060ff;#a060ff;#ff60ff;#ff60a0;#ff6060");
            return gradient;
        }
    }

    void ProgressBar::ctor(SongLoader::RuntimeSongLoader* runtimeSongLoader, GlobalNamespace::StandardLevelDetailViewController* levelDetailViewController) {
        _runtimeSongLoader = runtimeSongLoader;
        _levelDetailViewController = levelDetailViewController;

        _pos = UnityEngine::Vector3(0, 0.05f, 3);
        _rot = UnityEngine::Vector3(90, 0, 0);
        _scale = UnityEngine::Vector3(0.02f, 0.02f, 0.0f);

        _canvasScale = UnityEngine::Vector2(100, 50);
        _authorNamePos = UnityEngine::Vector2(10, 31);
        _headerPos = UnityEngine::Vector2(0, 15);
        _headerSize = UnityEngine::Vector2(120, 20);
        _pluginSize = UnityEngine::Vector2(120, 20);

        HeaderText = "Loading songs <size=60%><mspace=0.35em>[0000/0000]</mspace></size>";
        PluginText = "SongCore Loader";

        _headerTextSize = 15.0f;
        _pluginTextSize = 9.0f;

        _pluginTextPos = UnityEngine::Vector2(20, -10);

        _loadingBarSize = UnityEngine::Vector2(120, 10);
        _bgColor = UnityEngine::Color(0, 0, 0, 0.2f);

        _gradient = GetGradient();
    }

    void ProgressBar::Initialize() {
        _playButtonAction = BSML::MakeSystemAction<UnityW<GlobalNamespace::StandardLevelDetailViewController>>(
            std::function<void(UnityW<GlobalNamespace::StandardLevelDetailViewController>)>(
                [this](UnityW<GlobalNamespace::StandardLevelDetailViewController>){
                    _canvasDisplayTimer = std::nullopt;
                    _showingMessage = false;
                }
            )
        );

        _practiceButtonAction = BSML::MakeSystemAction<UnityW<GlobalNamespace::StandardLevelDetailViewController>, GlobalNamespace::BeatmapLevel*>(
            std::function<void(UnityW<GlobalNamespace::StandardLevelDetailViewController>, GlobalNamespace::BeatmapLevel*)>(
                [this](UnityW<GlobalNamespace::StandardLevelDetailViewController>, GlobalNamespace::BeatmapLevel*){
                    _canvasDisplayTimer = std::nullopt;
                    _showingMessage = false;
                }
            )
        );

        _levelDetailViewController->add_didPressActionButtonEvent(_playButtonAction);
        _levelDetailViewController->add_didPressPracticeButtonEvent(_practiceButtonAction);

        _runtimeSongLoader->SongsWillRefresh += {&ProgressBar::RuntimeSongLoaderOnSongRefresh, this};
        _runtimeSongLoader->SongsLoaded += {&ProgressBar::RuntimeSongLoaderOnSongLoaded, this};

        auto gameObject = UnityEngine::GameObject::New_ctor();
        UnityEngine::Object::DontDestroyOnLoad(gameObject);

        gameObject->transform->position = _pos;
        gameObject->transform->eulerAngles = _rot;
        gameObject->transform->localScale = _scale;

        _canvas = gameObject->AddComponent<UnityEngine::Canvas *>();
        _canvasGroup = gameObject->AddComponent<UnityEngine::CanvasGroup*>();
        _canvasGroup->alpha = 0.0f;
        _canvas->renderMode = UnityEngine::RenderMode::WorldSpace;
        _canvas->enabled = true;

        auto rect = _canvas->transform.cast<UnityEngine::RectTransform>();
        rect->sizeDelta = _canvasScale;

        _pluginNameText = BSML::Lite::CreateText(_canvas->transform.cast<UnityEngine::RectTransform>(), PluginText, _pluginTextSize, _pluginTextPos);
        rect = _pluginNameText->transform.cast<UnityEngine::RectTransform>();
        rect->SetParent(_canvas->transform, false);
        rect->anchoredPosition = _pluginTextPos;
        rect->sizeDelta = _pluginSize;
        _pluginNameText->text = PluginText;
        _pluginNameText->fontSize = _pluginTextSize;
        _pluginNameText->set_alignment(::TMPro::TextAlignmentOptions::Right);

        _headerText = BSML::Lite::CreateText(_canvas->transform.cast<UnityEngine::RectTransform>(), HeaderText, _headerPos);
        rect = _headerText->transform.cast<UnityEngine::RectTransform>();
        rect->SetParent(_canvas->transform, false);
        rect->anchoredPosition = _headerPos;
        rect->sizeDelta = _headerSize;
        _headerText->text = HeaderText;
        _headerText->fontSize = _headerTextSize;
        _pluginNameText->set_alignment(::TMPro::TextAlignmentOptions::Left);

        _loadingBg = UnityEngine::GameObject::New_ctor("Background")->AddComponent<UnityEngine::UI::Image *>();
        rect = _loadingBg->transform.cast<UnityEngine::RectTransform>();
        rect->SetParent(_canvas->transform, false);
        rect->sizeDelta = _loadingBarSize;
        _loadingBg->color = _bgColor;

        _loadingBar = UnityEngine::GameObject::New_ctor("Loading Bar")->AddComponent<UnityEngine::UI::Image *>();
        rect = _loadingBar->transform.cast<UnityEngine::RectTransform>();
        rect->SetParent(_canvas->transform, false);
        rect->sizeDelta = _loadingBarSize;
        auto tex = UnityEngine::Texture2D::get_whiteTexture();
        auto sprite = UnityEngine::Sprite::Create(tex, UnityEngine::Rect(0, 0, tex->width, tex->height), {1, 1}, 100, 1);
        _loadingBar->sprite = sprite;
        _loadingBar->type = UnityEngine::UI::Image::Type::Filled;
        _loadingBar->fillMethod = UnityEngine::UI::Image::FillMethod::Horizontal;
        _loadingBar->color = UnityEngine::Color(1, 1, 1, 0.5f);

        if (_runtimeSongLoader->AreSongsLoaded) {
            RuntimeSongLoaderOnSongLoaded(_runtimeSongLoader->AllLevels);
        } else if (_runtimeSongLoader->AreSongsRefreshing) {
            RuntimeSongLoaderOnSongRefresh();
        }
    }

    void ProgressBar::ShowMessage(std::string message) {
        _headerText->text = message;
        _canvas->enabled = true;
        _showingMessage = true;
        ShowCanvasForSeconds(5);
    }

    void ProgressBar::ShowMessage(std::string message, float time) {
        _headerText->text = message;
        _canvas->enabled = true;
        _showingMessage = true;
        ShowCanvasForSeconds(5);
    }

    void ProgressBar::ShowCanvasForSeconds(float time) {
        _canvasDisplayTimer = time;
    }

    void ProgressBar::HideCanvas() {
        _canvasDisplayTimer = std::nullopt;
        _showingMessage = false;
    }

    void ProgressBar::RuntimeSongLoaderOnSongRefresh() {
        _showingMessage = true;
        _updateSongCounter = true;
        _canvas->enabled = true;
        _headerText->text = HeaderText;
        _canvasDisplayTimer = std::nullopt;
    }

    void ProgressBar::RuntimeSongLoaderOnSongLoaded(std::span<SongLoader::CustomBeatmapLevel* const> customLevels) {
        _showingMessage = true;
        _updateSongCounter = false;
        _headerText->text = fmt::format("Loaded Songs! <size=60%><mspace=0.35em>[{:04d} total]</mspace></size>", customLevels.size());
        _beGay = true;
        _gradient = GetGradient();
        ShowCanvasForSeconds(5);
    }

    void ProgressBar::Tick() {
        if (!_canvas || !_canvas->enabled) return;

        float deltaTime = UnityEngine::Time::get_deltaTime();
        if (_canvasDisplayTimer.has_value()) {
            _canvasDisplayTimer.value() -= deltaTime;
            if (_canvasDisplayTimer.value() < 0) {
                _canvasDisplayTimer = std::nullopt;
                _showingMessage = false;
            }
        }

        if (_canvasGroup) {
            float delta = 4 * UnityEngine::Time::get_deltaTime();
            if (_showingMessage) { // raise up to 1
                _canvasGroup->alpha = std::min<float>(_canvasGroup->alpha + delta, 1.0f);
            } else { // decrease down to 0
                _canvasGroup->alpha = std::max<float>(_canvasGroup->alpha - delta, 0.0f);
            }

            if (_canvasGroup->alpha == 0) {
                _canvas->enabled = false;
                _beGay = false; // :pensive:
            }
        }

        if (_updateSongCounter) {
            _headerText->text = fmt::format("Loading songs <size=60%><mspace=0.35em>[{:04d}/{:04d}]</mspace></size>", _runtimeSongLoader->LoadedSongs, _runtimeSongLoader->TotalSongs);
        }

        _loadingBar->fillAmount = _runtimeSongLoader->Progress;

        UpdateLoadingBarColor();
    }

    void ProgressBar::Dispose() {
        if (_canvas && _canvas->m_CachedPtr.m_value) {
            UnityEngine::Object::Destroy(_canvas->gameObject);
        }
        _canvas = nullptr;
        _canvasGroup = nullptr;

        _runtimeSongLoader->SongsWillRefresh -= {&ProgressBar::RuntimeSongLoaderOnSongRefresh, this};
        _runtimeSongLoader->SongsLoaded -= {&ProgressBar::RuntimeSongLoaderOnSongLoaded, this};

        _levelDetailViewController->remove_didPressActionButtonEvent(_playButtonAction);
        _levelDetailViewController->remove_didPressPracticeButtonEvent(_practiceButtonAction);
    }

    void ProgressBar::UpdateLoadingBarColor() {
        if (_beGay) { // do crime
            _gayTime += UnityEngine::Time::get_deltaTime() * 0.3f;
            auto c32 =_gradient->Sample(_gayTime);
            _loadingBar->color = {
                c32.r / 256.0f,
                c32.g / 256.0f,
                c32.b / 256.0f,
                1.0f
            };
        } else {
            _loadingBar->color = {1, 1, 1, 0.5f};
        }
    }

    void ProgressBar::DisableImmediately() {
        _canvasDisplayTimer = std::nullopt;
        _showingMessage = false;
        _beGay = false; // :pensive:

        if (_canvas) _canvas->enabled = false;
        if (_canvasGroup) _canvasGroup->alpha = 0.0f;
    }
}
