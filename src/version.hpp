#include "farversion.hpp"

// PLUGIN_BUILD is defined on compiler command line. Set version number in CMakeLists.txt
// project(gitbranch VERSION x.y.Z) where Z is value for PLUGIN_BUILD

#define PLUGIN_DESC L"Show git branch at command propmt"
#define PLUGIN_NAME L"GitBranch"
#define PLUGIN_FILENAME L"gitbranch.dll"
#define PLUGIN_AUTHOR L"smithx"
#define PLUGIN_VERSION MAKEFARVERSION(FARMANAGERVERSION_MAJOR,FARMANAGERVERSION_MINOR,FARMANAGERVERSION_REVISION,PLUGIN_BUILD,VS_RELEASE)
