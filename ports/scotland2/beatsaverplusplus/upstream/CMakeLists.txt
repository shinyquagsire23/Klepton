cmake_minimum_required(VERSION 3.22)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/quest.cmake)
project(beatsaverplusplus VERSION ${PACKAGE_VERSION})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

set(SRC_DIR ${CMAKE_CURRENT_LIST_DIR}/src)
set(INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/include)

file(GLOB_RECURSE c_files ${SRC_DIR}/*.c)
file(GLOB_RECURSE cpp_files ${SRC_DIR}/*.cpp)

add_library(
    beatsaverplusplus
    SHARED
    ${c_files}
    ${cpp_files}
)

file(GLOB so_files ${EXTERN_DIR}/libs/lib*.so)
file(GLOB a_files ${EXTERN_DIR}/libs/lib*.a)

target_link_libraries(
    beatsaverplusplus
    PRIVATE
    log
    z
    ${EXTERN_DIR}/libs/libzip.so
    ${EXTERN_DIR}/libs/libpaper2_scotland2.so
    ${EXTERN_DIR}/libs/libweb-utils.so
)

target_include_directories(beatsaverplusplus PRIVATE ${INCLUDE_DIR})
target_include_directories(beatsaverplusplus PRIVATE ${SHARED_DIR})
target_include_directories(beatsaverplusplus PRIVATE ${EXTERN_DIR}/includes)
target_include_directories(beatsaverplusplus SYSTEM PRIVATE ${EXTERN_DIR}/includes/paper2_scotland2/shared/utfcpp/source)
target_include_directories(beatsaverplusplus SYSTEM PRIVATE ${EXTERN_DIR}/includes/fmt/fmt/include/)
target_include_directories(beatsaverplusplus SYSTEM PRIVATE ${EXTERN_DIR}/includes/beatsaber-hook/shared/rapidjson/include/)
target_include_directories(beatsaverplusplus SYSTEM PRIVATE ${EXTERN_DIR}/includes/libil2cpp/il2cpp/libil2cpp)
target_include_directories(beatsaverplusplus SYSTEM PRIVATE ${EXTERN_DIR}/includes/bs-cordl/include)
target_compile_options(beatsaverplusplus PUBLIC -fdeclspec -Wno-invalid-offsetof)
target_compile_definitions(beatsaverplusplus PRIVATE FMT_HEADER_ONLY)
target_compile_definitions(beatsaverplusplus PRIVATE MOD_ID="${PACKAGE_NAME}")
target_compile_definitions(beatsaverplusplus PRIVATE VERSION="${PACKAGE_VERSION}")
target_compile_definitions(beatsaverplusplus PRIVATE UNITY_2021)

add_custom_command(
    TARGET beatsaverplusplus POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
    ${CMAKE_CURRENT_BINARY_DIR}/debug
    COMMENT "Ensure debug dir exists"
)

add_custom_command(
    TARGET beatsaverplusplus POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rename
    ${CMAKE_CURRENT_BINARY_DIR}/libbeatsaverplusplus.so ${CMAKE_CURRENT_BINARY_DIR}/debug/libbeatsaverplusplus.so
    COMMENT "move normal binary into debug folder"
)

add_custom_command(
    TARGET beatsaverplusplus POST_BUILD
    COMMAND ${CMAKE_STRIP} -g -S -d --strip-all
    ${CMAKE_CURRENT_BINARY_DIR}/debug/libbeatsaverplusplus.so -o ${CMAKE_CURRENT_BINARY_DIR}/libbeatsaverplusplus.so
    COMMENT "Strip binary"
)

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/debug)
foreach(bin_file IN LISTS so_list a_list)
    cmake_path(GET bin_file FILENAME file_name)

    add_custom_command(
        TARGET beatsaverplusplus POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${bin_file} ${CMAKE_CURRENT_BINARY_DIR}/debug/${file_name}
        COMMENT "Copy so files for ndk stack"
    )

    add_custom_command(
        TARGET beatsaverplusplus POST_BUILD
        COMMAND ${CMAKE_STRIP} -g -S -d --strip-all
        ${bin_file} -o ${CMAKE_CURRENT_BINARY_DIR}/${file_name}
        COMMENT "Strip debug symbols from the dependencies"
    )
endforeach()
