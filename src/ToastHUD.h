// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Transient "now showing <overlay>" banner shown briefly when VR focus swaps
// between overlays, so the user knows which one they're looking at. Rendered in
// a dedicated, non-interactive ImGui context into a HUD-mode client's panel, so
// the existing HUD compositing pass draws it on top of whichever overlay is
// focused. Same pattern as HUDDemo.

#pragma once

#include <string>
#include <vector>

#include "ImGuiVRHelperInput.h"  // InputCombo

struct ID3D11RenderTargetView;

namespace ImGuiVRHelper::ToastHUD
{
	/// Render the banner with `text` top-center into the HUD-mode panel `rtv` at
	/// `alpha` in [0,1] (for fade-out). Lazily creates the context + DX11 backend.
	/// No-op if `rtv` is null, `text` is empty, or D3D isn't ready.
	/// Vertical placement comes from Settings::toastTopFraction (configurable);
	/// fontScale = per-window font scale (on top of the context's global scale).
	void Render(ID3D11RenderTargetView* rtv, const std::string& text, float alpha,
		float fontScale = 1.8f);

	/// Render the startup welcome banner into `rtv` at `alpha`: a title line plus
	/// "Open menu:" followed by `openKeys` color-coded per controller (matching
	/// the controller map), then the Shift+F4 hint. When `pauseHint` is set, also
	/// notes the menu only opens while the game is paused. Lazily creates context.
	void RenderWelcome(ID3D11RenderTargetView* rtv, float alpha,
		const std::vector<ImGuiVRHelperPluginAPI::InputCombo>& openKeys, bool pauseHint);

	/// Destroy the context + DX11 backend. Safe if never initialized.
	void Shutdown();
}
