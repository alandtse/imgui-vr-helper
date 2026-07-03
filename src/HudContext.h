// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#pragma once

#include "Globals.h"
#include "HudDisplay.h"
#include "Theme.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>

namespace ImGuiVRHelper
{
	// All HUD layers scale together off one factor: ImGui's own defaults (style metrics AND its
	// embedded font) are tuned as a matched set for desktop use, so scaling one without the other
	// throws proportion off -- padding/borders sized for the old scale now read as too thin or thick
	// next to text sized for a different one. kHudHeightScale is that single knob; kImGuiDefaultFontSize
	// (13px) is ImGui's own embedded-font baseline, so a real TTF baked at
	// kImGuiDefaultFontSize * kHudHeightScale lands at the same apparent size ScaleAllSizes(kHudHeightScale)
	// already targets for everything else.
	inline constexpr float kHudHeightScale = 2.0f;
	inline constexpr float kImGuiDefaultFontSize = 13.0f;

	// Loads a real TTF into the given atlas at kImGuiDefaultFontSize * kHudHeightScale, rather than
	// leaving io.Fonts on ImGui's embedded default and relying on FontGlobalScale to enlarge it --
	// that just magnifies the existing low-res bitmap glyphs, which is what read as pixelated text in
	// the HUD layers. No font ships with the helper, so this falls back to bundled-Windows system
	// fonts (always present, no new asset to ship). Sister project FloatingDamageNG hit the same issue
	// for its floating numbers and fixed it the same way. Returns whether a real font loaded, so the
	// caller can skip FontGlobalScale in that case (the TTF is already baked at the target size;
	// stretching it further would just reintroduce the blur this exists to fix) and only keep that
	// multiplier as a fallback for the embedded bitmap font.
	inline bool LoadHudFont(ImGuiIO& io)
	{
		std::vector<std::string> paths;
		// GetEnvironmentVariableA("WINDIR", ...) isn't guaranteed to be set (it can be unset or
		// overridden in unusual environments); GetWindowsDirectoryA is the actual WinAPI guarantee.
		char windir[MAX_PATH]{};
		if (GetWindowsDirectoryA(windir, MAX_PATH) > 0) {
			paths.push_back(std::format("{}\\Fonts\\segoeui.ttf", windir));
			paths.push_back(std::format("{}\\Fonts\\arial.ttf", windir));
		}
		for (const auto& path : paths) {
			if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
				continue;
			}
			if (ImFont* font = io.Fonts->AddFontFromFileTTF(path.c_str(), kImGuiDefaultFontSize * kHudHeightScale)) {
				io.FontDefault = font;
				return true;
			}
		}
		logs::warn("No TTF font found; HUD text falls back to the embedded bitmap font (will look pixelated when scaled).");
		return false;
	}

	// Creates a dedicated, non-interactive ImGui context for one HUD layer and
	// initializes its DX11 backend, applying the shared HUD look (Community Shaders
	// theme + kHudHeightScale-matched style and font) and the configured panel DisplaySize. The
	// new context is left current; returns it, or nullptr if the backend failed (the
	// context is cleaned up — caller retries next frame). `tag` names the layer in
	// the failure log. Centralizing this is what stops a HUD layer from silently
	// diverging on the look (e.g. dropping the theme). Call only once D3D is ready.
	inline ImGuiContext* CreateHudContext(const char* tag)
	{
		IMGUI_CHECKVERSION();
		ImGuiContext* ctx = ImGui::CreateContext();
		ImGui::SetCurrentContext(ctx);

		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.LogFilename = nullptr;
		ApplyHudDisplayMetrics(io);
		const bool realFontLoaded = LoadHudFont(io);

		Theme::Apply(ImGui::GetStyle());
		ImGui::GetStyle().ScaleAllSizes(kHudHeightScale);
		// The real TTF is already baked at kHudHeightScale's target size (see LoadHudFont); stretching
		// it further via FontGlobalScale would just blur it again. Keep the multiplier only as a
		// fallback for the tiny embedded bitmap font when no system font could be found, matched to
		// the same kHudHeightScale so the fallback is at least internally consistent with the style.
		io.FontGlobalScale = realFontLoaded ? 1.0f : kHudHeightScale;

		auto& d3d = Globals::GetD3D();
		if (!ImGui_ImplDX11_Init(d3d.device, d3d.context)) {
			logs::error("{}: ImGui_ImplDX11_Init failed", tag);
			ImGui::DestroyContext(ctx);
			return nullptr;
		}
		return ctx;
	}
}
