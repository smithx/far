cmake_minimum_required(VERSION 3.18)

project(gitbranch VERSION 3.0.21)

add_compile_options(/MP /std:c++17)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Functions to help placing dependencies into "ext" folder in Visual studio.
function(get_all_targets _result _dir)
    get_property(_subdirs DIRECTORY "${_dir}" PROPERTY SUBDIRECTORIES)
    foreach(_subdir IN LISTS _subdirs)
        get_all_targets(${_result} "${_subdir}")
    endforeach()
    get_property(_sub_targets DIRECTORY "${_dir}" PROPERTY BUILDSYSTEM_TARGETS)
    set(${_result} ${${_result}} ${_sub_targets} PARENT_SCOPE)
endfunction()

function(add_subdirectory_with_folder _folder_name _dir)
    add_subdirectory(${_dir} ${ARGN})
    get_all_targets(_targets "${_dir}")
    foreach(_target IN LISTS _targets)
        get_target_property(target_type ${_target} TYPE)
        get_target_property(target_name ${_target} NAME)
        if (${target_type} STREQUAL INTERFACE_LIBRARY)
          #message("not setting folder for INTERFACE_LIBRARY ${target_name}")
        else()
          #message("seting folder ${_folder_name} for target ${target_name}")
          set_target_properties(
            ${_target}
            PROPERTIES FOLDER "${_folder_name}")
        endif()
    endforeach()
endfunction()

set_property(GLOBAL PROPERTY USE_FOLDERS TRUE)

add_subdirectory(ext)
add_subdirectory(src)

