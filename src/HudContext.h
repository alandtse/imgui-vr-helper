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

		Theme::Apply(ImGui::GetStyle());
		ImGui::GetStyle().ScaleAllSizes(2.0f);
		io.FontGlobalScale = 1.5f;

		auto& d3d = Globals::GetD3D();
		if (!ImGui_ImplDX11_Init(d3d.device, d3d.context)) {
			logs::error("{}: ImGui_ImplDX11_Init failed", tag);
			ImGui::DestroyContext(ctx);
			return nullptr;
		}
		return ctx;
	}
}
