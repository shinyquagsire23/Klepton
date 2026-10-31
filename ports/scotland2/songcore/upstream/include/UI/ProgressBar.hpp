#pragma once

#include "SongLoader/CustomBeatmapLevel.hpp"
#include "SongLoader/RuntimeSongLoader.hpp"
#include "custom-types/shared/macros.hpp"
#include "lapiz/shared/macros.hpp"
#include "bsml/shared/BSML.hpp"
#include "Zenject/IInitializable.hpp"
#include "System/IDisposable.hpp"
#include "Zenject/ITickable.hpp"
#include "UnityEngine/Canvas.hpp"
#include "UnityEngine/CanvasGroup.hpp"
#include "TMPro/TMP_Text.hpp"

#include "System/Action_1.hpp"
#include "System/Action_2.hpp"
#include "GlobalNamespace/StandardLevelDetailViewController.hpp"
#include "UnityEngine/Vector2.hpp"
#include "UnityEngine/Vector3.hpp"
#include "UnityEngine/Rect.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/Color.hpp"
#include "UnityEngine/UI/Image.hpp"
#include "UnityEngine/Sprite.hpp"
#include "UnityEngine/RenderMode.hpp"

#include "SongLoader/RuntimeSongLoader.hpp"
#include "bsml/shared/BSML/Components/TextGradientUpdater.hpp"

DECLARE_CLASS_CODEGEN_INTERFACES(SongCore::UI, ProgressBar, System::Object, Zenject::IInitializable*, System::IDisposable*, Zenject::ITickable *) {
    DECLARE_CTOR(ctor, SongLoader::RuntimeSongLoader* runtimeSongLoader, GlobalNamespace::StandardLevelDetailViewController* levelDetailViewController);
    DECLARE_INSTANCE_FIELD(SongLoader::RuntimeSongLoader*, _runtimeSongLoader);
    DECLARE_INSTANCE_FIELD(GlobalNamespace::StandardLevelDetailViewController*, _levelDetailViewController);

    using PlayButtonAction = System::Action_1<UnityW<GlobalNamespace::StandardLevelDetailViewController>>;
    using PracticeButtonAction = System::Action_2<UnityW<GlobalNamespace::StandardLevelDetailViewController>, GlobalNamespace::BeatmapLevel*>;

    DECLARE_INSTANCE_FIELD_PRIVATE(PlayButtonAction*, _playButtonAction);
    DECLARE_INSTANCE_FIELD_PRIVATE(PracticeButtonAction*, _practiceButtonAction);

    DECLARE_INSTANCE_FIELD_PRIVATE(UnityEngine::Canvas*, _canvas);
    DECLARE_INSTANCE_FIELD_PRIVATE(UnityEngine::CanvasGroup*, _canvasGroup);
    DECLARE_INSTANCE_FIELD_PRIVATE(TMPro::TMP_Text*, _authorNameText);
    DECLARE_INSTANCE_FIELD_PRIVATE(TMPro::TMP_Text*, _pluginNameText);
    DECLARE_INSTANCE_FIELD_PRIVATE(TMPro::TMP_Text*, _headerText);
    DECLARE_INSTANCE_FIELD_PRIVATE(UnityEngine::UI::Image*, _loadingBg);
    DECLARE_INSTANCE_FIELD_PRIVATE(UnityEngine::UI::Image*, _loadingBar);

    DECLARE_INSTANCE_FIELD(UnityEngine::Vector3, _pos);
    DECLARE_INSTANCE_FIELD(UnityEngine::Vector3, _rot);
    DECLARE_INSTANCE_FIELD(UnityEngine::Vector3, _scale);

    DECLARE_INSTANCE_FIELD(UnityEngine::Vector2, _canvasScale);
    DECLARE_INSTANCE_FIELD(UnityEngine::Vector2, _authorNamePos);
    DECLARE_INSTANCE_FIELD(UnityEngine::Vector2, _headerPos);
    DECLARE_INSTANCE_FIELD(UnityEngine::Vector2, _headerSize);
    DECLARE_INSTANCE_FIELD(UnityEngine::Vector2, _pluginSize);

    DECLARE_INSTANCE_FIELD(UnityEngine::Vector2, _pluginTextPos);

    DECLARE_INSTANCE_FIELD(UnityEngine::Vector2, _loadingBarSize);
    DECLARE_INSTANCE_FIELD(UnityEngine::Color, _bgColor);

    UnityEngine::Coroutine* _disableCanvasRoutine;

    std::string HeaderText;
    std::string PluginText;

    float _headerTextSize;
    float _pluginTextSize;
    std::optional<float> _canvasDisplayTimer;

    bool _updateSongCounter;
    bool _showingMessage;
    bool _beGay;
    float _gayTime;
    BSML::Gradient* _gradient;

    void ShowMessage(std::string message, float time);
    void ShowMessage(std::string message);

    void RuntimeSongLoaderOnSongRefresh();
    void RuntimeSongLoaderOnSongLoaded(std::span<SongLoader::CustomBeatmapLevel* const> customLevels);
    void ShowCanvasForSeconds(float time);
    void HideCanvas();
    void UpdateLoadingBarColor();

    void DisableImmediately();

    DECLARE_OVERRIDE_METHOD_MATCH(void, Initialize, &::Zenject::IInitializable::Initialize);
    DECLARE_OVERRIDE_METHOD_MATCH(void, Dispose, &::System::IDisposable::Dispose);
    DECLARE_OVERRIDE_METHOD_MATCH(void, Tick, &::Zenject::ITickable::Tick);
};