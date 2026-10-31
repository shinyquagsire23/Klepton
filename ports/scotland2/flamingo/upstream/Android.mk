# Copyright (C) 2009 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.


LOCAL_PATH := $(call my-dir)

TARGET_ARCH_ABI := arm64-v8a

rwildcard=$(wildcard $1$2) $(foreach d,$(wildcard $1*),$(call rwildcard,$d/,$2))

# Build the beatsaber-hook shared library, SPECIFICALLY VERSIONED!
include $(CLEAR_VARS)
LOCAL_MODULE := bs-hook
LOCAL_EXPORT_C_INCLUDES := ./extern/beatsaber-hook/shared/
LOCAL_SRC_FILES := ./extern/beatsaber-hook/obj/local/arm64-v8a/libbeatsaber-hook_1_2_3.so
LOCAL_CPP_FEATURES += exceptions
include $(PREBUILT_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := modloader
LOCAL_EXPORT_C_INCLUDES := ./extern/modloader/shared/
LOCAL_SRC_FILES := ./extern/libmodloader.so
include $(PREBUILT_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := flamingo_0_1_0
LOCAL_SRC_FILES += $(call rwildcard,src/,*.cpp)
LOCAL_SRC_FILES += $(call rwildcard,extern/beatsaber-hook/src/inline-hook,*.cpp)
LOCAL_SRC_FILES += $(call rwildcard,extern/beatsaber-hook/src/inline-hook,*.c)
LOCAL_SHARED_LIBRARIES += bs-hook
LOCAL_SHARED_LIBRARIES += modloader
LOCAL_LDLIBS += -llog
LOCAL_CFLAGS += -I'C:/Users/Sc2ad/Desktop/Code/Android Modding/il2cpp/il2cpp_2019.3.15f1/libil2cpp' -DID='"flamingo"' -DVERSION='"0.1.0"' -isystem"./extern" -isystem"./include" -I'./shared' -I'./extern'
LOCAL_CPPFLAGS += -std=c++2a -Wall -Werror -Wno-unused-function
LOCAL_C_INCLUDES += ./include ./src
# LOCAL_CFLAGS	 += -D'RELEASE_BUILD'
# LOCAL_SRC_FILES  += src/main.cpp
include $(BUILD_SHARED_LIBRARY)
