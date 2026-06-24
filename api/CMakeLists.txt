# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Consumable CMake target for the ImGuiVRHelper *client* API.
#
# The helper plugin itself builds with xmake (see the repo root). This file
# exists only so client mods can depend on the LGPL `api/` surface with a clear,
# pinned upstream reference instead of copying the files in. Typical use from a
# client's CMakeLists (the client already provides CommonLibSSE, Dear ImGui, the
# DX11 backend and nlohmann_json — the API compiles against those):
#
#   include(FetchContent)
#   FetchContent_Declare(
#       ImGuiVRHelper
#       GIT_REPOSITORY https://github.com/alandtse/imgui-vr-helper.git
#       GIT_TAG        <commit-or-tag>
#       GIT_SUBMODULES ""        # api/ needs none of the helper's submodules
#       SOURCE_SUBDIR  api       # configure only this file, not the xmake root
#   )
#   FetchContent_MakeAvailable(ImGuiVRHelper)
#   target_link_libraries(MyPlugin PRIVATE ImGuiVRHelper::api)
#
# It is an INTERFACE library on purpose: the handshake stub and the SDK header
# are compiled/included inside the consumer's target, so they pick up the
# consumer's CommonLib / ImGui / json rather than requiring this project to
# resolve them.

cmake_minimum_required(VERSION 3.21)
project(ImGuiVRHelperAPI LANGUAGES CXX)

add_library(ImGuiVRHelperAPI INTERFACE)
add_library(ImGuiVRHelper::api ALIAS ImGuiVRHelperAPI)

target_sources(ImGuiVRHelperAPI INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/ImGuiVRHelperAPI.cpp")

target_include_directories(ImGuiVRHelperAPI INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}")

target_compile_features(ImGuiVRHelperAPI INTERFACE cxx_std_23)
