// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Smoke test for the kClientFlag_HUDMode pipeline. Renders a synthetic
// HUD overlay (Lorem Ipsum window + shapes) into a helper-owned RTV so
// the user can confirm the HUD compositing path works end-to-end before
// any real client connects against the API.
//
// Owns its own ImGui context — separate from SettingsUI's — so the demo
// content doesn't leak into the settings panel or vice versa. Per-context
// backend data was added to ImGui_ImplDX11 in 1.86 (we vendor 1.92), so
// two contexts each calling ImGui_ImplDX11_Init() coexist cleanly.

#pragma once

struct ID3D11RenderTargetView;

namespace ImGuiVRHelper::HUDDemo
{
	/// Lazy-init the demo's ImGui context + DX11 backend on first call.
	/// Returns false if D3D isn't ready yet. Idempotent.
	bool EnsureInitialized();

	/// Render the demo content into `rtv` (the panel's real pixel size is
	/// `panelWidth`x`panelHeight`, which may be supersampled). Must be called
	/// when Settings::showHUDDemo is on (HelperImpl::DispatchFrame's job to
	/// gate). Saves and restores the current ImGui context + the bound RTV.
	void Render(ID3D11RenderTargetView* rtv, unsigned int panelWidth, unsigned int panelHeight);

	/// Falling-edge cleanup: clear `rtv` to transparent {0,0,0,0} so
	/// the previous frame's demo pixels don't keep getting composited
	/// after the user toggles the demo off. Cheap; just a single
	/// ClearRenderTargetView call.
	void ClearToTransparent(ID3D11RenderTargetView* rtv);

	/// Tear down ImGui context + DX11 backend. Currently never called;
	/// the demo's resources persist for the helper's lifetime.
	void Shutdown();
}
