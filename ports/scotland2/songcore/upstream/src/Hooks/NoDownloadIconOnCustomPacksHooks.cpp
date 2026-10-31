#include "hooking.hpp"
#include "logging.hpp"

#include "GlobalNamespace/AnnotatedBeatmapLevelCollectionCell.hpp"
#include "SongLoader/CustomLevelPack.hpp"

// sets the download icon visibility to false if this is a custom pack
MAKE_AUTO_HOOK_MATCH(AnnotatedBeatmapLevelCollectionCell_RefreshAvailabilityAsync, &GlobalNamespace::AnnotatedBeatmapLevelCollectionCell::RefreshAvailabilityAsync, void, GlobalNamespace::AnnotatedBeatmapLevelCollectionCell* self, ::GlobalNamespace::IEntitlementModel* contentModel) {
    AnnotatedBeatmapLevelCollectionCell_RefreshAvailabilityAsync(self, contentModel);
    if (il2cpp_utils::try_cast<SongCore::SongLoader::CustomLevelPack>(self->_beatmapLevelPack).has_value()) {
        self->SetDownloadIconVisible(false);
    }
}
