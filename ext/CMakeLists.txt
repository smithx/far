
set(WINHTTP           FALSE CACHE BOOL "Don't use WinHTTP")
set(USE_HTTPS         FALSE CACHE BOOL "Don't use HTTPS")
set(BUILD_SHARED_LIBS FALSE CACHE BOOL "Don't build static libs for libgit2 and spdlog")
set(BUILD_CLAR        FALSE CACHE BOOL "Don't build libgit2_clar.exe")
set(ENABLE_TRACE      FALSE CACHE BOOL "No tracing")
set(USE_SSH           FALSE CACHE BOOL "No SSH")
set(USE_NSEC          FALSE CACHE BOOL "No nanosecond precision needed")
add_subdirectory_with_folder("ext" libgit2)
target_include_directories(git2 INTERFACE libgit2/include)

add_subdirectory_with_folder("ext" spdlog)

set_target_properties(pcre spdlog zlib git2 git2internal http-parser PROPERTIES EXCLUDE_FROM_ALL TRUE)

add_library(far INTERFACE)
target_include_directories(far INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/FarManager/plugins/common/unicode)