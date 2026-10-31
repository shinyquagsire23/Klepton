#include "BSML/Tags/LeaderboardTag.hpp"
#include "GlobalNamespace/LeaderboardTableView.hpp"
#include "GlobalNamespace/LeaderboardTableCell.hpp"
#include "GlobalNamespace/LoadingControl.hpp"
#include "VRUIControls/VRGraphicRaycaster.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/Object.hpp"
#include "Helpers/getters.hpp"
#include "TMPro/TextMeshProUGUI.hpp"
#include "GlobalNamespace/PlatformLeaderboardViewController.hpp"

using namespace UnityEngine;

namespace BSML {
    static BSMLNodeParser<LeaderboardTag> leaderboardTagParser({"leaderboard", "custom-leaderboard"});

    GlobalNamespace::LeaderboardTableView* get_leaderboardTemplate() {
        static SafePtrUnity<GlobalNamespace::LeaderboardTableView> leaderboardTemplate;
        if (!leaderboardTemplate) {
            leaderboardTemplate = Helpers::GetDiContainer()->Resolve<GlobalNamespace::PlatformLeaderboardViewController*>()->_leaderboardTableView;
        }
        return leaderboardTemplate.ptr();
    }

    UnityEngine::GameObject* LeaderboardTag::CreateObject(UnityEngine::Transform* parent) const {
        auto table = Object::Instantiate(get_leaderboardTemplate(), parent, false);
        table->set_name("BSMLLeaderboard");
        table->_cellPrefab->_scoreText->set_enableWordWrapping(false);
        table->GetComponent<VRUIControls::VRGraphicRaycaster*>()->_physicsRaycaster = Helpers::GetPhysicsRaycasterWithCache();
        for (auto tableCell : table->GetComponentsInChildren<GlobalNamespace::LeaderboardTableCell*>())
            UnityEngine::Object::Destroy(tableCell->get_gameObject());

        auto loadingControl = table->GetComponentInChildren<GlobalNamespace::LoadingControl*>();
        if (loadingControl) loadingControl->Hide();

        return table->get_gameObject();
    }
}
