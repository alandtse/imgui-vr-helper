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
	// Loads a real TTF into the given atlas at a large base pixel size, rather than
	// leaving io.Fonts on ImGui's embedded default (~13px) and relying on
	// FontGlobalScale to enlarge it -- that just magnifies the existing low-res
	// bitmap glyphs, which is what read as pixelated text in the HUD layers. No font
	// ships with the helper, so this falls back to bundled-Windows system fonts
	// (always present, no new asset to ship). Sister project FloatingDamageNG hit
	// the same issue for its floating numbers and fixed it the same way.
	//
	// Baked at 48px -- the size CreateHudContext's own 1.5x FontGlobalScale would
	// otherwise have stretched a 32px bake to -- because applying that scale on top
	// of an already-rasterized real font just re-introduces the same blur this is
	// meant to fix. Returns whether a real font loaded, so the caller can skip
	// FontGlobalScale in that case and only keep it as a fallback multiplier for the
	// embedded bitmap font.
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
			if (ImFont* font = io.Fonts->AddFontFromFileTTF(path.c_str(), 48.0f)) {
				io.FontDefault = font;
				return true;
			}
		}
		logs::warn("No TTF font found; HUD text falls back to the embedded bitmap font (will look pixelated when scaled).");
		return false;
	}

	// Creates a dedicated, non-interactive ImGui context for one HUD layer and
	// initializes its DX11 backend, applying the shared HUD look (Community Shaders
	// theme + 2x style scale + 1.5x font) and the configured panel DisplaySize. The
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
		ImGui::GetStyle().ScaleAllSizes(2.0f);
		// The real TTF is already baked at the intended on-screen size (see LoadHudFont); stretching
		// it further via FontGlobalScale would just blur it again. Keep the multiplier only as a
		// fallback for the tiny embedded bitmap font when no system font could be found.
		io.FontGlobalScale = realFontLoaded ? 1.0f : 1.5f;

		auto& d3d = Globals::GetD3D();
		if (!ImGui_ImplDX11_Init(d3d.device, d3d.context)) {
			logs::error("{}: ImGui_ImplDX11_Init failed", tag);
			ImGui::DestroyContext(ctx);
			return nullptr;
		}
		return ctx;
	}
}
