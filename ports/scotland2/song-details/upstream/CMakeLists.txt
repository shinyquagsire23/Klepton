cmake_minimum_required(VERSION 3.22)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/quest.cmake)

# get the vcpkg dir from env variables
if(EXISTS $ENV{VCPKG_ROOT})
    set(VCPKG_ROOT $ENV{VCPKG_ROOT})
else()
    MESSAGE(ERROR "Please define the environment variable VCPKG_ROOT with the root to your vcpkg install!")
endif()

# vcpkg config. Must be before project
message("VCPKG $ENV{VCPKG_ROOT}")
set(VCPKG_TARGET_TRIPLET arm64-android)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE ${CMAKE_ANDROID_NDK}/build/cmake/android.toolchain.cmake)
set(VCPKG_MANIFEST_DIR ${CMAKE_SOURCE_DIR})
set(VCPKG_INSTALLED_DIR ${CMAKE_SOURCE_DIR}/vcpkg_installed)
string(REPLACE "\\" "/" VCPKG_ROOT_WINDOWS_FIX $ENV{VCPKG_ROOT})
set(CMAKE_TOOLCHAIN_FILE ${VCPKG_ROOT_WINDOWS_FIX}/scripts/buildsystems/vcpkg.cmake)

project(${PACKAGE_ID} VERSION ${PACKAGE_VERSION})

# c++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN 1)

# define that stores the actual source directory
set(SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(COMPILE_ID songdetails)

# compile options used
add_compile_options(-frtti -fPIE -fPIC -fexceptions)

# Stop leaking library symbols
add_link_options(-Wl,--exclude-libs,ALL)

if(${CMAKE_BUILD_TYPE} STREQUAL "RELEASE" OR ${CMAKE_BUILD_TYPE} STREQUAL "RelWithDebInfo" OR ${CMAKE_BUILD_TYPE} STREQUAL "MinSizeRel")
    
    # Better optimizations
    add_compile_options(-O3)

    # LTO
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    add_compile_options(-flto)
endif()

# compile definitions used
add_compile_definitions(VERSION=\"${PACKAGE_VERSION}\")
add_compile_definitions(MOD_ID=\"${PACKAGE_ID}\")
add_compile_definitions(FMT_HEADER_ONLY)

# DEFINE UNITY_2021
add_compile_definitions(UNITY_2021)

# recursively get all src files
file(GLOB_RECURSE cpp_file_list ${SOURCE_DIR}/*.cpp)
file(GLOB_RECURSE c_file_list ${SOURCE_DIR}/*.c)

# add all src files to compile
add_library(
    ${COMPILE_ID}
    SHARED
    ${cpp_file_list}
    ${c_file_list}
    ${CMAKE_SOURCE_DIR}/src/SongProto.pb.cc # it ends in .cc so add it manually
)

target_include_directories(${COMPILE_ID} PRIVATE "${VCPKG_ROOT}/installed/arm64-android/include")

find_package(unofficial-brotli CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)

# add src dir as include dir
target_include_directories(${COMPILE_ID} PRIVATE ${SOURCE_DIR})
# add include dir as include dir
target_include_directories(${COMPILE_ID} PRIVATE ${INCLUDE_DIR})
# add shared dir as include dir
target_include_directories(${COMPILE_ID} PUBLIC ${SHARED_DIR})

target_include_directories(${COMPILE_ID} PRIVATE ${EXTERN_DIR}/includes)
target_include_directories(${COMPILE_ID} SYSTEM PRIVATE ${EXTERN_DIR}/includes/libil2cpp/il2cpp/libil2cpp)
target_include_directories(${COMPILE_ID} SYSTEM PRIVATE ${EXTERN_DIR}/includes/fmt/fmt/include/)

target_link_libraries(
    ${COMPILE_ID}
    PRIVATE
    -llog
    -lz
    unofficial::brotli::brotlidec
    protobuf::libprotobuf
    ${EXTERN_DIR}/libs/libcurl.a
    ${EXTERN_DIR}/libs/libpaper2_scotland2.so
)

add_custom_command(TARGET ${COMPILE_ID} POST_BUILD
            COMMAND ${CMAKE_STRIP} -g -S -d --strip-all
            "lib${COMPILE_ID}.so" -o "stripped_lib${COMPILE_ID}.so"
            COMMENT "Strip debug symbols done on final binary.")

add_custom_command(TARGET ${COMPILE_ID} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory debug
        COMMENT "Make directory for debug symbols"
        )

add_custom_command(TARGET ${COMPILE_ID} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E rename lib${COMPILE_ID}.so debug/lib${COMPILE_ID}.so
            COMMENT "Rename the lib to debug_ since it has debug symbols"
            )

add_custom_command(TARGET ${COMPILE_ID} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E rename stripped_lib${COMPILE_ID}.so lib${COMPILE_ID}.so
            COMMENT "Rename the stripped lib to regular"
            )

foreach(so_file ${so_list})
    cmake_path(GET so_file FILENAME file)

    add_custom_command(TARGET ${COMPILE_ID} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${so_file} debug/${file}
        COMMENT "Copy so files for ndk stack"
    )

    add_custom_command(TARGET ${COMPILE_ID} POST_BUILD
            COMMAND ${CMAKE_STRIP} -g -S -d --strip-all ${so_file} -o ${file}
            COMMENT "Strip debug symbols from the dependencies")
endforeach()

foreach(a_file ${a_list})
cmake_path(GET a_file FILENAME file)

add_custom_command(TARGET ${COMPILE_ID} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy ${a_file} debug/${file}
    COMMENT "Copy a files for ndk stack")
endforeach()
