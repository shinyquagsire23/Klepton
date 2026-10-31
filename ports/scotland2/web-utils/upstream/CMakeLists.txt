cmake_minimum_required(VERSION 3.22)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/quest.cmake)
project(web-utils VERSION ${PACKAGE_VERSION})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN 1)

set(SRC_DIR ${CMAKE_CURRENT_LIST_DIR}/src)
set(INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/include)

file(GLOB_RECURSE c_files ${SRC_DIR}/*.c)
file(GLOB_RECURSE cpp_files ${SRC_DIR}/*.cpp)

# Stop leaking library symbols
add_link_options(-Wl,--exclude-libs,ALL)

add_library(
    web-utils
    SHARED
    ${c_files}
    ${cpp_files}
)

target_compile_options(web-utils PRIVATE -Wno-invalid-offsetof)
target_compile_options(web-utils PRIVATE -DNEED_UNSAFE_CSHARP)
target_compile_options(web-utils PRIVATE -fdeclspec)
target_compile_options(web-utils PRIVATE -DUNITY_2021)
target_compile_options(web-utils PRIVATE -DHAS_CODEGEN)
target_compile_options(web-utils PRIVATE -DFMT_HEADER_ONLY)
target_compile_options(web-utils PRIVATE -DMOD_ID="WebUtils")
target_compile_options(web-utils PRIVATE -DVERSION="${PACKAGE_VERSION}")

target_include_directories(web-utils PRIVATE ${INCLUDE_DIR})
target_include_directories(web-utils PRIVATE ${SHARED_DIR})
target_include_directories(web-utils PRIVATE ${EXTERN_DIR}/includes/bs-cordl/include)
target_include_directories(web-utils PRIVATE ${EXTERN_DIR}/includes)
target_include_directories(web-utils PRIVATE ${CMAKE_CURRENT_LIST_DIR}/vcpkg_installed/arm64-android/include)
target_include_directories(web-utils SYSTEM PRIVATE ${EXTERN_DIR}/includes/paper/shared/utfcpp/source)
target_include_directories(web-utils SYSTEM PRIVATE ${EXTERN_DIR}/includes/fmt/fmt/include/)
target_include_directories(web-utils SYSTEM PRIVATE ${EXTERN_DIR}/includes/libil2cpp/il2cpp/libil2cpp)
target_include_directories(web-utils SYSTEM PRIVATE ${EXTERN_DIR}/includes/beatsaber-hook/shared/rapidjson/include/)
target_compile_options(web-utils PUBLIC -fdeclspec)
target_compile_definitions(web-utils PRIVATE FMT_HEADER_ONLY)

file(GLOB so_files ${EXTERN_DIR}/libs/lib*.so)
file(GLOB a_files ${EXTERN_DIR}/libs/lib*.a)

target_link_libraries(web-utils PRIVATE -lz -llog ${so_files} ${a_files})

add_custom_command(
    TARGET web-utils POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
    ${CMAKE_CURRENT_BINARY_DIR}/debug
    COMMENT "Ensure debug dir exists"
)

add_custom_command(
    TARGET web-utils POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rename
    ${CMAKE_CURRENT_BINARY_DIR}/libweb-utils.so ${CMAKE_CURRENT_BINARY_DIR}/debug/libweb-utils.so
    COMMENT "move normal binary into debug folder"
)

add_custom_command(
    TARGET web-utils POST_BUILD
    COMMAND ${CMAKE_STRIP} -g -S -d --strip-all
    ${CMAKE_CURRENT_BINARY_DIR}/debug/libweb-utils.so -o ${CMAKE_CURRENT_BINARY_DIR}/libweb-utils.so
    COMMENT "Strip binary"
)

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/debug)
foreach(bin_file IN LISTS so_list a_list)
    cmake_path(GET bin_file FILENAME file_name)

    add_custom_command(
        TARGET web-utils POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${bin_file} ${CMAKE_CURRENT_BINARY_DIR}/debug/${file_name}
        COMMENT "Copy so files for ndk stack"
    )

    add_custom_command(
        TARGET web-utils POST_BUILD
        COMMAND ${CMAKE_STRIP} -g -S -d --strip-all
        ${bin_file} -o ${CMAKE_CURRENT_BINARY_DIR}/${file_name}
        COMMENT "Strip debug symbols from the dependencies"
    )
endforeach()
