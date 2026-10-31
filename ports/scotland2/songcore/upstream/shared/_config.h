#pragma once

#define SONGCORE_EXPORT __attribute__((visibility("default")))
#ifdef __cplusplus
#define SONGCORE_EXPORT_FUNC extern "C" SONGCORE_EXPORT
#else
#define SONGCORE_EXPORT_FUNC SONGCORE_EXPORT
#endif 