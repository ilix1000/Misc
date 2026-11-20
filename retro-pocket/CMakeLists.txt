cmake_minimum_required(VERSION 3.16)
# The app's built-in version (shown after updates) comes from main/version.h.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/main/version.h" _ver REGEX "#define FW_VERSION")
string(REGEX REPLACE ".*\"(.*)\".*" "\\1" PROJECT_VER "${_ver}")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(retro_pocket)
