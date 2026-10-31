#if MAKE_DOCS
#include "hooking.hpp"

#include "GlobalNamespace/LevelSelectionFlowCoordinator.hpp"
#include "BSML/Parsing/BSMLDocParser.hpp"

bool didIt = false;
MAKE_AUTO_HOOK_MATCH(LevelSelectionFlowCoordinator_DidActivate, &GlobalNamespace::LevelSelectionFlowCoordinator::DidActivate, void, GlobalNamespace::LevelSelectionFlowCoordinator* self, bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling) {
    LevelSelectionFlowCoordinator_DidActivate(self, firstActivation, addedToHierarchy, screenSystemEnabling);

    // do it once per game launch, if we just happen to ever give a docs build, it's only going to freeze once
    if (!didIt) {
        didIt = true;
        BSML::BSMLDocParser::PrintDocs();
    }
}
#endif