// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#pragma once

#include "Overlay.h"

#include <algorithm>
#include <imgui.h>

namespace ImGuiVRHelper
{
	// Resolves the configured logical panel base resolution into w/h, falling back
	// to the default. Shared so the RTV allocation, the io DisplaySize, and the
	// VRAM estimate all agree on one source of truth.
	inline void ResolveBaseDims(int& w, int& h)
	{
		const auto& s = Overlay::State::GetSingleton().settings;
		w = s.baseWidth > 0 ? s.baseWidth : Overlay::Config::kOverlayWidth;
		h = s.baseHeight > 0 ? s.baseHeight : Overlay::Config::kOverlayHeight;
	}

	// The configured HUD supersample factor, clamped to the supported range.
	inline int HudSupersample()
	{
		return std::clamp(Overlay::State::GetSingleton().settings.hudSupersample, 1,
			Overlay::Config::kMaxHUDSupersample);
	}

	// Configures a HUD ImGui context's logical size and supersample from the
	// current overlay settings. EVERY dedicated HUD context (toasts, the rebind
	// popup, the settings panel, the calibration demo) must call this before it
	// positions windows: they place by fraction of io.DisplaySize, so a context
	// that bakes in the default 1920x1080 instead of the configured base
	// resolution mispositions everything on a non-default panel. Keeping it in one
	// place is what stops a new HUD layer from silently forgetting it.
	//
	// panelWidth/panelHeight: the actual RTV pixel size, when the caller has it.
	// Given, the framebuffer scale is the real panel/base ratio; omitted, it falls
	// back to the configured supersample factor (panel == base x supersample).
	inline void ApplyHudDisplayMetrics(ImGuiIO& io, unsigned int panelWidth = 0, unsigned int panelHeight = 0)
	{
		int baseWi, baseHi;
		ResolveBaseDims(baseWi, baseHi);
		const float baseW = static_cast<float>(baseWi);
		const float baseH = static_cast<float>(baseHi);
		io.DisplaySize = ImVec2(baseW, baseH);
		if (panelWidth && panelHeight) {
			io.DisplayFramebufferScale =
				ImVec2(static_cast<float>(panelWidth) / baseW, static_cast<float>(panelHeight) / baseH);
		} else {
			const float ss = static_cast<float>(HudSupersample());
			io.DisplayFramebufferScale = ImVec2(ss, ss);
		}
	}
}
