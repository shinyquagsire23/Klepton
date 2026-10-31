#pragma once

#include "HMUI/TableCell.hpp"
#include "HMUI/TableView.hpp"
#include "UnityEngine/MonoBehaviour.hpp"
#include "UnityEngine/Sprite.hpp"
#include "custom-types/shared/macros.hpp"

DECLARE_CLASS_CODEGEN(PlaylistCore, CustomTableCell, HMUI::TableCell) {
    DECLARE_OVERRIDE_METHOD_MATCH(
        void, SelectionDidChange, &HMUI::SelectableCell::SelectionDidChange, HMUI::SelectableCell::TransitionType transitionType
    );
    DECLARE_OVERRIDE_METHOD_MATCH(
        void, HighlightDidChange, &HMUI::SelectableCell::HighlightDidChange, HMUI::SelectableCell::TransitionType transitionType
    );

   protected:
    std::function<void()> refreshVisualsFunc;
    std::function<void(UnityEngine::Sprite*, std::string)> initFunc;
    std::function<void(UnityEngine::Sprite*)> setSpriteFunc;
    std::function<void(std::string)> setTextFunc;

   public:
    void refreshVisuals() {
        refreshVisualsFunc();
    }
    void init(UnityEngine::Sprite * sprite, std::string text) {
        initFunc(sprite, text);
    }
    void setSprite(UnityEngine::Sprite * sprite) {
        setSpriteFunc(sprite);
    }
    void setText(std::string text) {
        setTextFunc(text);
    }
};

DECLARE_CLASS_CODEGEN_INTERFACES(PlaylistCore, CustomListSource, UnityEngine::MonoBehaviour, HMUI::TableView::IDataSource*) {
    DECLARE_INSTANCE_FIELD(Il2CppString*, reuseIdentifier);
    DECLARE_INSTANCE_FIELD(HMUI::TableView*, tableView);

    DECLARE_CTOR(ctor);
    DECLARE_DTOR(dtor);

    DECLARE_OVERRIDE_METHOD_MATCH(HMUI::TableCell*, CellForIdx, &HMUI::TableView::IDataSource::CellForIdx, HMUI::TableView * tableView, int idx);

    DECLARE_OVERRIDE_METHOD_MATCH(float, CellSize, &HMUI::TableView::IDataSource::CellSize);
    DECLARE_OVERRIDE_METHOD_MATCH(int, NumberOfCells, &HMUI::TableView::IDataSource::NumberOfCells);

   private:
    std::vector<UnityEngine::Sprite*> sprites;
    std::vector<std::string> texts;
    bool expandCell;
    System::Type* type;

   public:
    static void ScrollListLeft(CustomListSource * list, int numCells);
    static void ScrollListRight(CustomListSource * list, int numCells);

    void setType(System::Type * cellType);
    void addSprites(std::vector<UnityEngine::Sprite*> newSprites);
    void replaceSprites(std::vector<UnityEngine::Sprite*> newSprites);
    void clearSprites();
    void addTexts(std::vector<std::string> newTexts);
    void replaceTexts(std::vector<std::string> newTexts);
    void clearTexts();
    void clear();
    UnityEngine::Sprite* getSprite(int index);
    std::string getText(int index);
};
