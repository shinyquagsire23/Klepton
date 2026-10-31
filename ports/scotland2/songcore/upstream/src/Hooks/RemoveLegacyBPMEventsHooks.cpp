#include "hooking.hpp"
#include "logging.hpp"

#include "BeatmapSaveDataVersion2_6_0AndEarlier/BeatmapSaveDataVersion2_6_0AndEarlier.hpp"

#include "System/Collections/Generic/IReadOnlyCollection_1.hpp"

// Event10 was briefly used as an official BPM change between 1.8.0 and 1.18.0,
// but it was never supported by custom mapping tools and later reused as a light event.
// The code to convert these events broke a lot of maps, so we are removing it here.
MAKE_AUTO_HOOK_MATCH(
    BeatmapSaveData_ConvertBeatmapSaveDataPreV2_5_0Inline,
    &BeatmapSaveDataVersion2_6_0AndEarlier::BeatmapSaveData::ConvertBeatmapSaveDataPreV2_5_0Inline,
    void,
    BeatmapSaveDataVersion2_6_0AndEarlier::BeatmapSaveData *self)
{
    // Use fake type to avoid conversion logic and to keep the original call so it can be hooked by other mods
    const BeatmapSaveDataCommon::BeatmapEventType placeholderType = 999;
    for (int i = 0; i < ((System::Collections::Generic::IReadOnlyCollection_1<BeatmapSaveDataVersion2_6_0AndEarlier::EventData *> *)self->events)->Count; i++)
    {
        auto event = self->events->Item[i];
        if (event->_type == BeatmapSaveDataCommon::BeatmapEventType::LegacyBpmEventType)
        {
            event->_type = placeholderType;
        }
    }

    BeatmapSaveData_ConvertBeatmapSaveDataPreV2_5_0Inline(self);

    for (int i = 0; i < ((System::Collections::Generic::IReadOnlyCollection_1<BeatmapSaveDataVersion2_6_0AndEarlier::EventData *> *)self->_events)->Count; i++)
    {
        auto event = self->_events->Item[i];
        if (event->_type == placeholderType)
        {
            event->_type = BeatmapSaveDataCommon::BeatmapEventType::LegacyBpmEventType;
        }
    }
}