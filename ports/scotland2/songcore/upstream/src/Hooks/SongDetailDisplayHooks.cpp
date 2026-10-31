#include "hooking.hpp"
#include "logging.hpp"

#include "GlobalNamespace/LevelListTableCell.hpp"
#include "SongLoader/CustomBeatmapLevel.hpp"
#include "UnityEngine/Vector3.hpp"
#include "UnityEngine/Transform.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "TMPro/TMP_Text.hpp"
#include <algorithm>

MAKE_AUTO_HOOK_MATCH(
    LevelListTableCell_SetDataFromLevelAsync,
    &GlobalNamespace::LevelListTableCell::SetDataFromLevelAsync,
    void,
    GlobalNamespace::LevelListTableCell* self,
    GlobalNamespace::BeatmapLevel* level,
    bool isFavorite,
    bool isPromoted,
    bool isUpdated,
    bool interactable
) {
    LevelListTableCell_SetDataFromLevelAsync(self, level, isFavorite, isPromoted, isUpdated, interactable);
    if (!level) return;

    self->_songBpmText->text = std::to_string((int)level->beatsPerMinute);
    auto customLevel = il2cpp_utils::try_cast<SongCore::SongLoader::CustomBeatmapLevel>(level);
    self->_songAuthorText->richText = true;

    std::vector<StringW> allAuthors;
    allAuthors.insert(allAuthors.begin(), level->allMappers->begin(), level->allMappers.end());
    allAuthors.insert(allAuthors.begin(), level->allLighters->begin(), level->allLighters.end());

    allAuthors.erase( unique( allAuthors.begin(), allAuthors.end() ), allAuthors.end() );

    if (!allAuthors.empty()) {
        for (auto& author : allAuthors) {
            author = author->Replace(u"<", u"<\u200B")->Replace(u">", u"\u200B>");
        }

        auto songAuthorName = level->songAuthorName;
        auto levelAuthorName = fmt::format("{}", fmt::join(allAuthors, ", "));

        auto color = "ff69b4";

        self->_songAuthorText->text = fmt::format("<size=80%>{}</size> <size=90%>[<color=#{}>{}</color>]</size>", songAuthorName, color, levelAuthorName);
    }

    auto size = self->_songNameText->rectTransform->sizeDelta;
    size.y = 6.5f;
    self->_songNameText->rectTransform->sizeDelta = size;

    if (isFavorite) {
        static auto favoriteSize = UnityEngine::Vector3(1.4,1.4,1.4);

        UnityEngine::Color color = {1, 1, 1, 1};
        if (customLevel.has_value()) color = {1, 0.4, 0.7, 0.7};
        self->_favoritesBadgeImage->color = color;
        self->_favoritesBadgeImage->transform->localScale = favoriteSize;
    }
}
