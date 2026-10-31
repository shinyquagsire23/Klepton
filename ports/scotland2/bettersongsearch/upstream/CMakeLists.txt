# include some defines automatically made by qpm
include(qpm_defines.cmake)

cmake_minimum_required(VERSION 3.21)
project(${COMPILE_ID})
include(${EXTERN_DIR}/includes/kaleb/shared/cmake/assets.cmake)

# Hot reload (DO NOT USE IN PRODUCTION)
# add_compile_options(-DHotReload)

# c++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED 20)

# define that stores the actual source directory
set(SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# compile options used
add_link_options(-Wl,--exclude-libs,ALL)
add_compile_options(-frtti -fPIE -fPIC -fexceptions -fvisibility=hidden)

if(${CMAKE_BUILD_TYPE} STREQUAL "RELEASE" OR ${CMAKE_BUILD_TYPE} STREQUAL "RelWithDebInfo" OR ${CMAKE_BUILD_TYPE} STREQUAL "MinSizeRel")

    # Better optimizations
    add_compile_options(-O3)

    # LTO
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    add_compile_options(-flto)
else()
    # Debug options
    add_compile_options(-O0 -g)
endif()

# get git info
execute_process(COMMAND git config user.name OUTPUT_VARIABLE GIT_USER)
execute_process(COMMAND git branch --show-current OUTPUT_VARIABLE GIT_BRANCH)
execute_process(COMMAND git rev-parse --short HEAD OUTPUT_VARIABLE GIT_COMMIT)
execute_process(COMMAND git diff-index --quiet HEAD RESULT_VARIABLE GIT_MODIFIED)

string(STRIP "${GIT_USER}" GIT_USER)
string(STRIP "${GIT_BRANCH}" GIT_BRANCH)
string(STRIP "${GIT_COMMIT}" GIT_COMMIT)
string(STRIP "${GIT_MODIFIED}" GIT_MODIFIED)

message(STATUS "GIT_USER: ${GIT_USER}")
message(STATUS "GIT_BRANCH: ${GIT_BRANCH}")
message(STATUS "GIT_COMMIT: 0x${GIT_COMMIT}")
message(STATUS "GIT_MODIFIED: ${GIT_MODIFIED}")

# set git defines
add_compile_definitions(GIT_USER=\"${GIT_USER}\")
add_compile_definitions(GIT_BRANCH=\"${GIT_BRANCH}\")
add_compile_definitions(GIT_COMMIT=0x${GIT_COMMIT})
add_compile_definitions(GIT_MODIFIED=${GIT_MODIFIED})

# compile definitions used
add_compile_definitions(VERSION=\"${MOD_VERSION}\")
add_compile_definitions(MOD_ID=\"${MOD_ID}\")
# add_compile_definitions(DEBUGB=1) # disable debug logs

# recursively get all src files
RECURSE_FILES(cpp_file_list ${SOURCE_DIR}/*.cpp)
RECURSE_FILES(cpp_file_test_list ./test/src/*.cpp)
RECURSE_FILES(c_file_list ${SOURCE_DIR}/*.c)

RECURSE_FILES(inline_hook_c ${EXTERN_DIR}/includes/beatsaber-hook/shared/inline-hook/*.c)
RECURSE_FILES(inline_hook_cpp ${EXTERN_DIR}/includes/beatsaber-hook/shared/inline-hook/*.cpp)

# add all src files to compile
add_library(
        ${COMPILE_ID}
        SHARED
        ${cpp_file_list}
        ${cpp_file_test_list}
        ${c_file_list}
        ${inline_hook_c}
        ${inline_hook_cpp}
)

target_include_directories(${COMPILE_ID} PRIVATE .)

target_include_directories(${COMPILE_ID} PRIVATE extern/includes/questui_components)

# add src dir as include dir
target_include_directories(${COMPILE_ID} PRIVATE ${SOURCE_DIR})
target_include_directories(${COMPILE_ID} PRIVATE test/src)

# add include dir as include dir
target_include_directories(${COMPILE_ID} PRIVATE ${INCLUDE_DIR})
target_include_directories(${COMPILE_ID} PRIVATE ./test/include)

# add shared dir as include dir
target_include_directories(${COMPILE_ID} PUBLIC ${SHARED_DIR})
# codegen includes
target_include_directories(${COMPILE_ID} PRIVATE ${EXTERN_DIR}/includes/${CODEGEN_ID}/include)

# add assets
add_assets(${COMPILE_ID}-assets STATIC ${CMAKE_CURRENT_LIST_DIR}/assets ${INCLUDE_DIR}/assets.hpp)
target_link_libraries(${COMPILE_ID} PRIVATE -llog -lz  ${COMPILE_ID}-assets)

# add extern stuff like libs and other includes
include(extern.cmake)

add_custom_command(TARGET ${COMPILE_ID} POST_BUILD
        COMMAND ${CMAKE_STRIP} -d --strip-all
        "lib${COMPILE_ID}.so" -o "stripped_lib${COMPILE_ID}.so"
        COMMENT "Strip debug symbols done on final binary.")

add_custom_command(TARGET ${COMPILE_ID} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory debug
        COMMENT "Rename the lib to debug_ since it has debug symbols"
        )

add_custom_command(TARGET ${COMPILE_ID} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rename lib${COMPILE_ID}.so debug/lib${COMPILE_ID}.so
        COMMENT "Rename the lib to debug_ since it has debug symbols"
        )

add_custom_command(TARGET ${COMPILE_ID} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rename stripped_lib${COMPILE_ID}.so lib${COMPILE_ID}.so
        COMMENT "Rename the stripped lib to regular"
        )
