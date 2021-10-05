add_library(gitbranch MODULE
    GitBranch.cpp
    GitBranch.rc
    GitBranchW.vc.def
    guid.hpp
    version.hpp)

target_link_libraries(gitbranch PRIVATE git2 spdlog::spdlog_header_only far)
target_compile_definitions(gitbranch PRIVATE PLUGIN_BUILD=${PROJECT_VERSION_PATCH})

set_property(
    TARGET gitbranch
    PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_GENERATOR ZIP)
set(CPACK_BINARY_ZIP ON)

install(TARGETS gitbranch
    LIBRARY DESTINATION GitBranch)

set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_CURRENT_BINARY_DIR};gitbranch;ALL;.")
include(CPack)
