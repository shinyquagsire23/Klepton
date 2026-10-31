#pragma once

#define BSML_EXPORT __attribute__((visibility("default")))
#ifdef __cplusplus
#define BSML_EXPORT_FUNC extern "C" BSML_EXPORT
#else
#define BSML_EXPORT_FUNC BSML_EXPORT
#endif
