#pragma once

#include "custom-types/shared/macros.hpp"
#include "custom-types/shared/coroutine.hpp"
#include "UnityEngine/MonoBehaviour.hpp"
#include "UnityEngine/Canvas.hpp"
#include "UnityEngine/UI/Image.hpp"
#include "TMPro/TMP_Text.hpp"

// type borrowed from https://github.com/darknight1050/QuestUI
DECLARE_CLASS_CODEGEN(BSML, ProgressBar, UnityEngine::MonoBehaviour) {
    DECLARE_INSTANCE_METHOD(void, OnDisable);
    DECLARE_INSTANCE_METHOD(void, Awake);
    DECLARE_INSTANCE_METHOD(void, Update);

    DECLARE_INSTANCE_FIELD(UnityEngine::Canvas*, canvas);
    DECLARE_INSTANCE_FIELD(TMPro::TMP_Text*, subText1);
    DECLARE_INSTANCE_FIELD(TMPro::TMP_Text*, subText2);
    DECLARE_INSTANCE_FIELD(TMPro::TMP_Text*, headerText);
    DECLARE_INSTANCE_FIELD(UnityEngine::UI::Image*, loadingBackground);
    DECLARE_INSTANCE_FIELD(UnityEngine::UI::Image*, loadingBar);
    DECLARE_INSTANCE_FIELD(bool, inited);
    DECLARE_INSTANCE_FIELD_PRIVATE(bool, showingMessage);

    private:
        custom_types::Helpers::Coroutine DisableCanvasRoutine(float time);
    public:
        void ShowMessage(StringW text, float time);
        void ShowMessage(StringW text);
        void SetProgress(float progress);
        void SetProgress(float progress, bool showBar);

        static ProgressBar* CreateProgressBar(UnityEngine::Vector3 position, UnityEngine::Vector3 scale, UnityEngine::Vector3 rotation, StringW mainText, StringW subText1 = "", StringW subText2 = "");
};