// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Color scheme matching Community Shaders. The helper is a standalone plugin and
// can't depend on CS, so the palette values are ported from CS's ThemeManager
// (neutral-gray foundation + dark, semi-transparent window background) and its
// per-controller color encoding (Utils/VRUtils).

#pragma once

#include <imgui.h>

#include "ImGuiVRHelperInput.h"  // InputDeviceType

namespace ImGuiVRHelper::Theme
{
	/// Apply the CS-matching color scheme to `style`. Sets only version-stable
	/// ImGuiCol_ entries so it builds against any recent Dear ImGui.
	void Apply(ImGuiStyle& style);

	/// Per-controller color, matching CS: Primary = yellow, Secondary = blue,
	/// Both = green, anything else white. Use it so a color consistently means
	/// "this controller" across the diagnostics + mapping UI.
	ImVec4 DeviceColor(ImGuiVRHelperPluginAPI::InputDeviceType device);

	/// Per-controller name ("Primary"/"Secondary"/"Both"; "?" otherwise). Shared
	/// so the welcome banner, rebind popup, and controller map can't drift.
	const char* DeviceName(ImGuiVRHelperPluginAPI::InputDeviceType device);
}
