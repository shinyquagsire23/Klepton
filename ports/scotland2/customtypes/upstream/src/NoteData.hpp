// NoteData.hpp
#include "beatsaber-hook/shared/utils/typedefs.h"

class BeatmapObjectData : public Il2CppObject {
    public:
    // private BeatmapObjectType <beatmapObjectType>k__BackingField
    // Offset: 0x10
    int beatmapObjectType;
    // private System.Single <time>k__BackingField
    // Offset: 0x14
    float time;
    // private System.Int32 <lineIndex>k__BackingField
    // Offset: 0x18
    int lineIndex;
    // private System.Int32 <id>k__BackingField
    // Offset: 0x1C
    int id;
};
#include "beatsaber-hook/shared/utils/il2cpp-type-check.hpp"
DEFINE_IL2CPP_ARG_TYPE(BeatmapObjectData*, "", "BeatmapObjectData");

class NoteData : public BeatmapObjectData {
    public:
    // private NoteType <noteType>k__BackingField
    // Offset: 0x20
    int noteType;
    // private NoteCutDirection <cutDirection>k__BackingField
    // Offset: 0x24
    int cutDirection;
    // private NoteLineLayer <noteLineLayer>k__BackingField
    // Offset: 0x28
    int noteLineLayer;
    // private NoteLineLayer <startNoteLineLayer>k__BackingField
    // Offset: 0x2C
    int startNoteLineLayer;
    // private System.Int32 <flipLineIndex>k__BackingField
    // Offset: 0x30
    int flipLineIndex;
    // private System.Single <flipYSide>k__BackingField
    // Offset: 0x34
    float flipYSide;
    // private System.Single <timeToNextBasicNote>k__BackingField
    // Offset: 0x38
    float timeToNextBasicNote;
    // private System.Single <timeToPrevBasicNote>k__BackingField
    // Offset: 0x3C
    float timeToPrevBasicNote;
};
DEFINE_IL2CPP_ARG_TYPE(NoteData*, "", "NoteData");
