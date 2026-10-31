#pragma once

#include "custom-types/shared/macros.hpp"
#include "UnityEngine/Transform.hpp"
#include "UnityEngine/UI/LayoutGroup.hpp"
#include "GlobalNamespace/GameplaySetupViewController.hpp"

#include "../Components/TabSelector.hpp"
#include "../Components/CustomListTableData.hpp"
#include "../Components/ModalView.hpp"
#include "GameplaySetupCell.hpp"
#include "MenuType.hpp"

DECLARE_CLASS_CODEGEN_INTERFACES(BSML, GameplaySetup, System::Object, HMUI::TableView::IDataSource*) {
    DECLARE_INSTANCE_FIELD(UnityEngine::UI::LayoutGroup*, layoutGroup);
    DECLARE_INSTANCE_FIELD(GlobalNamespace::GameplaySetupViewController*, gameplaySetupViewController);
    DECLARE_INSTANCE_FIELD(bool, listParsed);
    DECLARE_INSTANCE_FIELD(bool, _loaded);

    DECLARE_INSTANCE_FIELD(BSML::ModalView*, listModal);
    DECLARE_INSTANCE_FIELD(CustomListTableData*, modsList);
    DECLARE_INSTANCE_FIELD(ListW<System::Object*>, _menus);
    DECLARE_INSTANCE_FIELD(ListW<UnityEngine::Transform*>, _vanillaItems);

    DECLARE_INSTANCE_FIELD(HMUI::ImageView*, loading);
    DECLARE_INSTANCE_FIELD(BSML::TabSelector*, tabSelector);
    DECLARE_INSTANCE_FIELD(UnityEngine::Transform*, vanillaTab);
    DECLARE_INSTANCE_FIELD(UnityEngine::Transform*, modsTab);
    DECLARE_INSTANCE_FIELD(StringW, reuseIdentifier);

    DECLARE_INSTANCE_METHOD(ListW<System::Object*>, get_menus);
    DECLARE_INSTANCE_METHOD(ListW<UnityEngine::Transform*>, get_vanillaItems);
    DECLARE_INSTANCE_METHOD(bool, get_loaded);
    DECLARE_INSTANCE_METHOD(void, set_loaded, bool value);

    DECLARE_INSTANCE_METHOD(void, Setup);
    DECLARE_INSTANCE_METHOD(void, GameplaySetupDidActivate, bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling);
    DECLARE_INSTANCE_METHOD(void, GameplaySetupDidDeactivate, bool removedFromHierarchy, bool screenSystemDisabling);
    DECLARE_INSTANCE_METHOD(void, ClickedOffModal);
    DECLARE_INSTANCE_METHOD(void, ShowModal);

    DECLARE_INSTANCE_METHOD(GameplaySetupCell*, GetCell);
    DECLARE_OVERRIDE_METHOD_MATCH(float, CellSize, &HMUI::TableView::IDataSource::CellSize);
    DECLARE_OVERRIDE_METHOD_MATCH(int, NumberOfCells, &HMUI::TableView::IDataSource::NumberOfCells);
    DECLARE_OVERRIDE_METHOD_MATCH(HMUI::TableCell*, CellForIdx, &HMUI::TableView::IDataSource::CellForIdx, HMUI::TableView* tableView, int idx);

    DECLARE_DEFAULT_CTOR();
    public:
        bool AddTab(std::string_view name, std::string_view content_key, System::Object* host, MenuType menuType = MenuType::All);
        bool AddTab(System::Type* csType, std::string_view name, MenuType menuType = MenuType::All);
        bool AddTab(std::function<void(UnityEngine::GameObject*, bool)> didActivate, std::string_view name, MenuType menuType = MenuType::All);
        void SetTabVisibility(std::string_view name, bool isVisible);
        bool RemoveTab(std::string_view name);

        static GameplaySetup* get_instance();

        HMUI::TableView::IDataSource* i_DataSource();
    private:
        static SafePtr<GameplaySetup> instance;
};