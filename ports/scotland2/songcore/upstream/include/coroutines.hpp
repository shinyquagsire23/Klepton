#pragma once

#include "custom-types/shared/coroutine.hpp"
#include "GlobalNamespace/SharedCoroutineStarter.hpp"

#define StartCoroutine(coroutine) GlobalNamespace::SharedCoroutineStarter::get_instance()->StartCoroutine(custom_types::Helpers::CoroutineHelper::New(coroutine))