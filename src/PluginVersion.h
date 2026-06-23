#pragma once

// Plugin version string, built from the compile-time version defines (set in
// xmake.lua). Shared so the load log, settings window, and welcome banner all
// report the same version.
#define IMGUI_VR_HELPER_STR_HELPER(x) #x
#define IMGUI_VR_HELPER_STR(x) IMGUI_VR_HELPER_STR_HELPER(x)
#define IMGUI_VR_HELPER_VERSION_STRING                 \
	IMGUI_VR_HELPER_STR(IMGUI_VR_HELPER_VERSION_MAJOR) \
	"." IMGUI_VR_HELPER_STR(IMGUI_VR_HELPER_VERSION_MINOR) "." IMGUI_VR_HELPER_STR(IMGUI_VR_HELPER_VERSION_PATCH)
