#pragma once

#define BEATSAVER_PLUSPLUS_EXPORT __attribute__((visibility("default")))
#ifdef __cplusplus
#define BEATSAVER_PLUSPLUS_EXPORT_FUNC extern "C" BEATSAVER_PLUSPLUS_EXPORT
#else
#define BEATSAVER_PLUSPLUS_EXPORT_FUNC BEATSAVER_PLUSPLUS_EXPORT
#endif
