cmake_minimum_required(VERSION 3.22)

option(QUEST "Build for quest" ON)

# Globals
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

set(SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_compile_options(-frtti -fexceptions -fvisibility=hidden -fPIE -fPIC)

# Include. Include order matters!
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/utils.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/qpm.cmake)

# Enable hot reload on debug builds
if (NOT DEFINED HOT_RELOAD)
    if (${CMAKE_BUILD_TYPE} MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
        set(HOT_RELOAD 0)
    else ()
        set(HOT_RELOAD 1)
    endif ()
endif ()
add_compile_definitions(HOT_RELOAD=${HOT_RELOAD})
message(STATUS "HOT_RELOAD = ${HOT_RELOAD}")

if (${CMAKE_BUILD_TYPE} STREQUAL "RELEASE" OR ${CMAKE_BUILD_TYPE} STREQUAL "RelWithDebInfo" OR ${CMAKE_BUILD_TYPE} STREQUAL "MinSizeRel")
    # Better optimizations
    add_compile_options(-O3)

    # LTO
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    add_compile_options(-flto)
endif ()

if (${CMAKE_BUILD_TYPE} STREQUAL "DEBUG" OR ${CMAKE_BUILD_TYPE} STREQUAL "RelWithDebInfo")
    add_compile_options(-g)
endif ()

# Targets
if (QUEST)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/quest.cmake)
endif ()

# Post build
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/strip.cmake)

# stop symbols leaking
# TODO: Fix
# add_link_options(-Wl, --exclude-libs, ALL)
project(airbuds-search VERSION ${PACKAGE_VERSION})

add_compile_definitions(MOD_ID="${CMAKE_PROJECT_NAME}")
add_compile_definitions(VERSION="${CMAKE_PROJECT_VERSION}")

# Set COMPILE_ID for qpm purposes
set(COMPILE_ID ${CMAKE_PROJECT_NAME})

# recursively get all src files
RECURSE_FILES(cpp_file_list ${SOURCE_DIR}/*.cpp)
RECURSE_FILES(c_file_list ${SOURCE_DIR}/*.c)

add_library(
        ${CMAKE_PROJECT_NAME}
        SHARED
        ${cpp_file_list}
        ${c_file_list}
)

add_subdirectory(third-party)

# for inline hook
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE log crypto ssl httplib dl)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${INCLUDE_DIR})
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${SHARED_DIR})

# beatsaber hook inline hook

RECURSE_FILES(src_inline_hook_beatsaber_hook_local_extra_c ${EXTERN_DIR}/includes/beatsaber-hook/shared/inline-hook/*.c)
RECURSE_FILES(src_inline_hook_beatsaber_hook_local_extra_cpp ${EXTERN_DIR}/includes/beatsaber-hook/shared/inline-hook/*.cpp)

target_sources(${COMPILE_ID} PRIVATE ${src_inline_hook_beatsaber_hook_local_extra_c})
target_sources(${COMPILE_ID} PRIVATE ${src_inline_hook_beatsaber_hook_local_extra_cpp})

# Add assets
include(extern/includes/metacore/shared/assets.cmake)
