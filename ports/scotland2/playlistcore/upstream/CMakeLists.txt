# include some defines automatically made by qpm
include(qpm_defines.cmake)

cmake_minimum_required(VERSION 3.21)
project(${COMPILE_ID})

# c++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED 20)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# define that stores the actual source directory
set(SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

# compile options used
add_compile_options(-frtti -fexceptions)
add_compile_options(-O3)

# compile definitions used
add_compile_definitions(VERSION=\"${MOD_VERSION}\")
add_compile_definitions(MOD_ID=\"${MOD_ID}\")

string(LENGTH "${CMAKE_CURRENT_LIST_DIR}/" FOLDER_LENGTH)
add_compile_definitions("PAPER_ROOT_FOLDER_LENGTH=${FOLDER_LENGTH}")

# recursively get all src files
recurse_files(cpp_file_list ${SOURCE_DIR}/*.cpp)
recurse_files(c_file_list ${SOURCE_DIR}/*.c)

recurse_files(inline_hook_c ${EXTERN_DIR}/includes/beatsaber-hook/shared/inline-hook/*.c)
recurse_files(inline_hook_cpp ${EXTERN_DIR}/includes/beatsaber-hook/shared/inline-hook/*.cpp)

# add all src files to compile
add_library(
    ${COMPILE_ID} SHARED ${cpp_file_list} ${c_file_list} ${inline_hook_c} ${inline_hook_cpp}
)

# add include dir as include dir
target_include_directories(${COMPILE_ID} PRIVATE ${INCLUDE_DIR})

# add shared dir as include dir
target_include_directories(${COMPILE_ID} PUBLIC ${SHARED_DIR})

target_link_libraries(${COMPILE_ID} PRIVATE -llog)

# add extern stuff like libs and other includes
include(extern.cmake)

# include assets
include(extern/includes/metacore/shared/assets.cmake)

add_custom_command(
    TARGET ${COMPILE_ID}
    POST_BUILD
    COMMAND ${CMAKE_STRIP} -d --strip-all "lib${COMPILE_ID}.so" -o "stripped_lib${COMPILE_ID}.so"
    COMMENT "Strip debug symbols done on final binary."
)

add_custom_command(
    TARGET ${COMPILE_ID}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory debug
    COMMENT "Rename the lib to debug_ since it has debug symbols"
)

add_custom_command(
    TARGET ${COMPILE_ID}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rename lib${COMPILE_ID}.so debug/lib${COMPILE_ID}.so
    COMMENT "Rename the lib to debug_ since it has debug symbols"
)

add_custom_command(
    TARGET ${COMPILE_ID}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rename stripped_lib${COMPILE_ID}.so lib${COMPILE_ID}.so
    COMMENT "Rename the stripped lib to regular"
)
