#pragma once

#define MOD_EXPORT __attribute__((visibility("default")))
#define MOD_EXTERN_FUNC extern "C" MOD_EXPORT