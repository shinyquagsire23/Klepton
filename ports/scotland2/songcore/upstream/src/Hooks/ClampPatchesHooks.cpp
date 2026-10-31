#include "hooking.hpp"
#include "logging.hpp"

#include "GlobalNamespace/BeatmapObjectsInTimeRowProcessor.hpp"
#include "GlobalNamespace/SliderData.hpp"
#include "GlobalNamespace/NoteData.hpp"
#include "System/Collections/Generic/IReadOnlyCollection_1.hpp"
#include "System/Collections/Generic/IReadOnlyList_1.hpp"

void BeatmapObjectsInTimeRowProcessor_HandleCurrentTimeSliceAllNotesAndSlidersDidFinishTimeSliceTranspile(
    GlobalNamespace::BeatmapObjectsInTimeRowProcessor* self,
    GlobalNamespace::BeatmapObjectsInTimeRowProcessor::TimeSliceContainer_1<::GlobalNamespace::BeatmapDataItem*>* allObjectsTimeSlice,
    float nextTimeSliceTime
) {
    // clear all data
    auto notesInColumnsReusableProcessingListOfLists = self->_notesInColumnsReusableProcessingListOfLists;
    for (auto const &l: notesInColumnsReusableProcessingListOfLists) {
        l->Clear();
    }

    auto itemsReadOnly = allObjectsTimeSlice->items;
    auto count = itemsReadOnly->i___System__Collections__Generic__IReadOnlyCollection_1_T_()->Count;

    for (int i = 0; i < count; i++) {
        auto noteData = il2cpp_utils::try_cast<GlobalNamespace::NoteData>(itemsReadOnly->get_Item(i)).value_or(nullptr);
        if (!noteData) continue;

        // this is the transpile. we have to clamp the line index here to make the game not break
        ListW<GlobalNamespace::NoteData*> list {self->_notesInColumnsReusableProcessingListOfLists[std::clamp(noteData->lineIndex, 0, 3)]};

        // insert this notedata after next highest line layer
        bool addedToList = false;
        for (auto j = 0; j < list.size(); j++) {
            auto item = list[j];
            // there's a bug in the game that causes null notedatas somehow because of a divide by 0, rather than crash we're just going to skip the notes
            if (!item) continue;
            if (item->noteLineLayer > noteData->noteLineLayer) {
                list.insert_at(j, noteData);
                addedToList = true;
                break;
            }
        }

        if (!addedToList) list.push_back(noteData);
    }

    for (ListW<GlobalNamespace::NoteData*> list : notesInColumnsReusableProcessingListOfLists) {
        for (int noteLineLayer = 0; auto noteData : list) {
            // same null check for that bug just to be sure
            if (noteData) noteData->SetBeforeJumpNoteLineLayer(GlobalNamespace::NoteLineLayer(noteLineLayer++));
        }
    }

    for (int i = 0; i < count; i++) {
        auto sliderData = il2cpp_utils::try_cast<GlobalNamespace::SliderData>(itemsReadOnly->get_Item(i)).value_or(nullptr);
        if (!sliderData) continue;

        for (int j = 0; j < count; j++) {
            auto noteData = il2cpp_utils::try_cast<GlobalNamespace::NoteData>(itemsReadOnly->get_Item(j)).value_or(nullptr);
            if (!noteData) continue;
            if (GlobalNamespace::BeatmapObjectsInTimeRowProcessor::SliderHeadPositionOverlapsWithNote(sliderData, noteData)) {
                sliderData->SetHasHeadNote(true);
                sliderData->SetHeadBeforeJumpLineLayer(noteData->beforeJumpNoteLineLayer);

                if (sliderData->sliderType == GlobalNamespace::SliderData::Type::Burst) {
                    noteData->ChangeToBurstSliderHead();
                } else {
                    noteData->MarkAsSliderHead();
                }
            }
        }
    }

    for (int i = 0; i < count; i++) {
        auto sliderData1 = il2cpp_utils::try_cast<GlobalNamespace::SliderData>(itemsReadOnly->get_Item(i)).value_or(nullptr);
        if (!sliderData1) continue;

        for (int j = 0; j < count; j++) {
            auto sliderData2 = il2cpp_utils::try_cast<GlobalNamespace::SliderData>(itemsReadOnly->get_Item(j)).value_or(nullptr);
            if (!sliderData2) continue;
            if (sliderData1 == sliderData2) continue;

            if (GlobalNamespace::BeatmapObjectsInTimeRowProcessor::SliderHeadPositionOverlapsWithBurstTail(sliderData1, sliderData2)) {
                sliderData1->SetHasHeadNote(true);
                sliderData1->SetHeadBeforeJumpLineLayer(sliderData2->tailBeforeJumpLineLayer);
            }
        }

        for (int j = 0; j < count; j++) {
            auto sliderTailData = il2cpp_utils::try_cast<GlobalNamespace::BeatmapObjectsInTimeRowProcessor::SliderTailData>(itemsReadOnly->get_Item(j)).value_or(nullptr);
            if (!sliderTailData) continue;

            if (GlobalNamespace::BeatmapObjectsInTimeRowProcessor::SliderHeadPositionOverlapsWithBurstTail(sliderData1, sliderTailData->slider)) {
                sliderData1->SetHasHeadNote(true);
                sliderData1->SetHeadBeforeJumpLineLayer(sliderTailData->slider->tailBeforeJumpLineLayer);
            }
        }
    }

    for (int i = 0; i < count; i++) {
        auto sliderTailData = il2cpp_utils::try_cast<GlobalNamespace::BeatmapObjectsInTimeRowProcessor::SliderTailData>(itemsReadOnly->get_Item(i)).value_or(nullptr);
        if (!sliderTailData) continue;
        auto slider = sliderTailData->slider;

        for (int j = 0; j < count; j++) {
            auto noteData = il2cpp_utils::try_cast<GlobalNamespace::NoteData>(itemsReadOnly->get_Item(j)).value_or(nullptr);
            if (!noteData) continue;

            if (GlobalNamespace::BeatmapObjectsInTimeRowProcessor::SliderTailPositionOverlapsWithNote(slider, noteData)) {
                slider->SetHasTailNote(true);
                slider->SetTailBeforeJumpLineLayer(noteData->beforeJumpNoteLineLayer);
                noteData->MarkAsSliderTail();
            }
        }
    }
}

MAKE_AUTO_HOOK_ORIG_MATCH(BeatmapObjectsInTimeRowProcessor_HandleCurrentTimeSliceAllNotesAndSlidersDidFinishTimeSlice,
                &GlobalNamespace::BeatmapObjectsInTimeRowProcessor::HandleCurrentTimeSliceAllNotesAndSlidersDidFinishTimeSlice,
                void,
                GlobalNamespace::BeatmapObjectsInTimeRowProcessor* self,
                GlobalNamespace::BeatmapObjectsInTimeRowProcessor::TimeSliceContainer_1<::GlobalNamespace::BeatmapDataItem*>* allObjectsTimeSlice, float nextTimeSliceTime) {
    return BeatmapObjectsInTimeRowProcessor_HandleCurrentTimeSliceAllNotesAndSlidersDidFinishTimeSliceTranspile(self, allObjectsTimeSlice, nextTimeSliceTime);
}
