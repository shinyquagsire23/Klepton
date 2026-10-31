#include "hooking.hpp"
#include "logging.hpp"
#include "config.hpp"

#include "GlobalNamespace/BeatLineManager.hpp"

bool NoteSpawnLinesOverrideLevelIsCustom = false;
bool NoteSpawnLinesOverrideShowLines = false;

MAKE_AUTO_HOOK_ORIG_MATCH(BeatLineManager_HandleNoteWasSpawned, &GlobalNamespace::BeatLineManager::HandleNoteWasSpawned, void, GlobalNamespace::BeatLineManager* self, GlobalNamespace::NoteController* noteController) {
    if (!NoteSpawnLinesOverrideLevelIsCustom) return BeatLineManager_HandleNoteWasSpawned(self, noteController);

    if (NoteSpawnLinesOverrideShowLines) return BeatLineManager_HandleNoteWasSpawned(self, noteController);
}
