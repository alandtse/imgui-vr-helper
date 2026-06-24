// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include "pch.h"

#include "Theme.h"

namespace ImGuiVRHelper::Theme
{
	void Apply(ImGuiStyle& style)
	{
		ImVec4* c = style.Colors;

		// Foundation, ported from CS ThemeManager (Palette): dark + semi-
		// transparent window over the scene, white text, neutral-gray chrome.
		const ImVec4 bg{ 0.03f, 0.03f, 0.03f, 0.39f };   // Palette.Background
		const ImVec4 text{ 1.0f, 1.0f, 1.0f, 1.0f };     // Palette.Text
		const ImVec4 wborder{ 0.5f, 0.5f, 0.5f, 0.8f };  // Palette.WindowBorder
		const ImVec4 frame{ 0.4f, 0.4f, 0.4f, 0.7f };    // Palette.FrameBorder
		const ImVec4 sep{ 0.5f, 0.5f, 0.5f, 0.6f };      // Palette.Separator
		const ImVec4 grip{ 0.6f, 0.6f, 0.6f, 0.8f };     // Palette.ResizeGrip

		c[ImGuiCol_WindowBg] = bg;
		c[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
		c[ImGuiCol_PopupBg] = ImVec4(bg.x, bg.y, bg.z, 0.92f);
		c[ImGuiCol_Text] = text;
		c[ImGuiCol_TextDisabled] = ImVec4(text.x, text.y, text.z, 0.5f);  // CS DISABLED_TEXT_ALPHA
		c[ImGuiCol_Border] = wborder;
		c[ImGuiCol_Separator] = sep;
		c[ImGuiCol_SeparatorHovered] = ImVec4(sep.x, sep.y, sep.z, 0.9f);
		c[ImGuiCol_SeparatorActive] = ImVec4(sep.x, sep.y, sep.z, 1.0f);
		c[ImGuiCol_ResizeGrip] = grip;
		c[ImGuiCol_ResizeGripHovered] = ImVec4(grip.x, grip.y, grip.z, 1.0f);
		c[ImGuiCol_ResizeGripActive] = ImVec4(grip.x, grip.y, grip.z, 1.0f);
		c[ImGuiCol_CheckMark] = text;
		c[ImGuiCol_SliderGrab] = frame;
		c[ImGuiCol_SliderGrabActive] = ImVec4(frame.x, frame.y, frame.z, 1.0f);

		// Neutral-gray interactive surfaces, replacing ImGui's default blue so
		// the helper reads as CS-neutral.
		const ImVec4 rest{ 0.30f, 0.30f, 0.30f, 0.55f };
		const ImVec4 hover{ 0.45f, 0.45f, 0.45f, 0.75f };
		const ImVec4 active{ 0.55f, 0.55f, 0.55f, 0.90f };
		c[ImGuiCol_FrameBg] = frame;
		c[ImGuiCol_FrameBgHovered] = hover;
		c[ImGuiCol_FrameBgActive] = active;
		c[ImGuiCol_Button] = rest;
		c[ImGuiCol_ButtonHovered] = hover;
		c[ImGuiCol_ButtonActive] = active;
		c[ImGuiCol_Header] = rest;
		c[ImGuiCol_HeaderHovered] = hover;
		c[ImGuiCol_HeaderActive] = active;
		c[ImGuiCol_TitleBg] = ImVec4(0.06f, 0.06f, 0.06f, 0.90f);
		c[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.10f, 0.10f, 0.95f);
		c[ImGuiCol_Tab] = rest;
		c[ImGuiCol_TabHovered] = hover;
	}

	ImVec4 DeviceColor(ImGuiVRHelperPluginAPI::InputDeviceType device)
	{
		using D = ImGuiVRHelperPluginAPI::InputDeviceType;
		switch (device) {
		case D::Primary:
			return ImVec4(1.0f, 1.0f, 0.0f, 1.0f);  // yellow
		case D::Secondary:
			return ImVec4(0.0f, 0.5f, 1.0f, 1.0f);  // blue
		case D::Both:
			return ImVec4(0.0f, 1.0f, 0.0f, 1.0f);  // green
		default:
			return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);  // white
		}
	}

	const char* DeviceName(ImGuiVRHelperPluginAPI::InputDeviceType device)
	{
		using D = ImGuiVRHelperPluginAPI::InputDeviceType;
		switch (device) {
		case D::Primary:
			return "Primary";
		case D::Secondary:
			return "Secondary";
		case D::Both:
			return "Both";
		default:
			return "?";
		}
	}
}
