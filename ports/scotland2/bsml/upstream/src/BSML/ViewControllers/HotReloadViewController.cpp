#include "BSML/ViewControllers/HotReloadViewController.hpp"

#include "UnityEngine/GameObject.hpp"   

DEFINE_TYPE(BSML, HotReloadViewController);

using namespace UnityEngine;

void BSML::HotReloadViewController::Awake() {
    fileWatcher = get_gameObject()->AddComponent<HotReloadFileWatcher*>();
    fileWatcher->host = this;
}