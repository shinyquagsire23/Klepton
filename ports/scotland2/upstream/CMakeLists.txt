if (NOT DEFINED BUILD_FOR)
    set(BUILD_FOR QUEST)
endif()

if (${BUILD_FOR} STREQUAL LINUX)
    set(CMAKE_ANDROID_NDK "h/")
endif()

# include some defines automatically made by qpm
include(qpm_defines.cmake)

if (${BUILD_FOR} STREQUAL LINUX)
    message("Building for Linux...")
    set(CMAKE_TOOLCHAIN_FILE "")
    set(CMAKE_SYSTEM_NAME ${CMAKE_HOST_SYSTEM_NAME})

    add_compile_definitions(LINUX_TEST)
endif()

cmake_minimum_required(VERSION 3.22)
project(${COMPILE_ID})

# c++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED 20)
if (${CMAKE_BUILD_TYPE} STREQUAL "RELEASE" OR ${CMAKE_BUILD_TYPE} STREQUAL "RelWithDebInfo")
    # LTO
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# define that stores the actual source directory
set(SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

# compile options used
add_compile_options(-fPIE -flto -fPIC -fno-rtti -fvisibility=hidden -Os -O2)
add_compile_definitions(MOD_VERSION=\"${MOD_VERSION}\")
add_compile_definitions(MOD_ID=\"${MOD_ID}\")
# Ensure we don't create any references through flamingo
add_compile_definitions(FLAMINGO_HEADER_ONLY)
add_compile_options(-Wall -Wextra -Werror -Wpedantic -Wno-gnu-zero-variadic-macro-arguments)
# add_link_options(-fuse-ld=lld)
# compile definitions used

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (DEFINED TEST_BUILD)
    MESSAGE(STATUS "Compiling with test defines")
endif()

RECURSE_FILES(cpp_file_list ${SOURCE_DIR}/*.cpp)

list(APPEND cpp_file_list
    ${EXTERN_DIR}/includes/flamingo/src/fixups.cpp
    ${EXTERN_DIR}/includes/flamingo/src/installer.cpp
    ${EXTERN_DIR}/includes/flamingo/src/page-allocator.cpp
)

add_library(
    ${COMPILE_ID} 
    SHARED
    ${cpp_file_list}
)

# add src dir as include dir
target_include_directories(${COMPILE_ID} PRIVATE ${SOURCE_DIR})
# add include dir as include dir
target_include_directories(${COMPILE_ID} PRIVATE ${INCLUDE_DIR})
# add shared dir as include dir
target_include_directories(${COMPILE_ID} PUBLIC ${SHARED_DIR})

include(extern.cmake)
MESSAGE(STATUS "extern added!")

# Specifically remove the libflamingo.so dependency, as we want to compile directly via extraFiles
get_target_property(LINKED_LIBS ${COMPILE_ID} LINK_LIBRARIES)
message("Libraries being linked: ${LINKED_LIBS}")
list(REMOVE_ITEM LINKED_LIBS ${EXTERN_DIR}/libs/libflamingo.so)
list(REMOVE_ITEM LINKED_LIBS ${EXTERN_DIR}/libs/libflamingo.debug.so)
message("After removal: ${LINKED_LIBS}")
set_property(TARGET ${COMPILE_ID} PROPERTY LINK_LIBRARIES ${LINKED_LIBS})
# Add include specifically for flamingo shared folder
target_include_directories(${COMPILE_ID} PRIVATE ${EXTERN_DIR}/includes/flamingo/shared)
# Add include specifically for flamingo build (to capstone directly)
target_include_directories(${COMPILE_ID} PRIVATE ${EXTERN_DIR}/includes/capstone/shared)


if (${BUILD_FOR} STREQUAL LINUX)
    message("Building tests")
    RECURSE_FILES(cpp_test_file_list ./test/*.cpp)

    set(testname BootstrapTest)

    add_executable(${testname} ${cpp_test_file_list})

    # add include dir as include dir
    target_include_directories(${testname} PRIVATE ${INCLUDE_DIR})
    target_include_directories(${testname} PRIVATE ./test/)
    # add shared dir as include dir
    target_include_directories(${testname} PUBLIC ${SHARED_DIR})

    target_link_libraries(${testname} PRIVATE ${COMPILE_ID} -ldl)

else()
    target_link_libraries(${COMPILE_ID} PRIVATE -llog -ldl)

add_custom_command(TARGET ${COMPILE_ID} POST_BUILD
        COMMAND ${CMAKE_STRIP} -g -S -d --strip-all
        "lib${COMPILE_ID}.so" -o "stripped_lib${COMPILE_ID}.so"
        COMMENT "Strip debug symbols done on final binary.")

add_custom_command(TARGET ${COMPILE_ID} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rename lib${COMPILE_ID}.so debug_lib${COMPILE_ID}.so
        COMMENT "Rename the lib to debug_ since it has debug symbols"
        )

add_custom_command(TARGET ${COMPILE_ID} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rename stripped_lib${COMPILE_ID}.so lib${COMPILE_ID}.so
        COMMENT "Rename the stripped lib to regular"
        )

endif()
