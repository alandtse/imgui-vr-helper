// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Helper's own settings UI. Owns an ImGui context that's separate from any
// client's context (different DLLs link their own ImGui). Renders a single
// settings window with VR-overlay configuration: offsets, scale, attach
// mode, drag toggle, wand pointing toggle.
//
// The helper exposes itself as its own client (via HelperImpl::EnsureSelfClient)
// so the existing per-client texture / focus / submit pipeline picks up its
// panel without special-casing in InSceneOverlay.

#pragma once

struct ImGuiContext;

namespace ImGuiVRHelper::SettingsUI
{
	/// Allocate the ImGui context, init ImGui_ImplDX11. Idempotent.
	/// Call after Globals::IsReady().
	bool Init();

	/// Tear down the ImGui context and DX11 backend.
	void Shutdown();

	/// True iff Init has succeeded.
	bool IsInitialized();

	/// Render one frame of the settings UI into the helper's panel RTV.
	/// Returns true iff the UI was visible and a frame was drawn.
	/// `dt` is seconds since previous Render call.
	bool Render(float dt);

	/// Toggle the settings window visible/hidden. Wired to a combo by
	/// HelperImpl on first frame.
	void Toggle();

	/// Returns true iff the settings window is currently visible —
	/// either toggled open via the hotkey/combo, or forced visible
	/// because the SteamVR dashboard is showing the helper's panel
	/// (see SetForceVisible).
	bool IsVisible();

	/// Force the settings window to render even when the user hasn't
	/// toggled it open. HelperImpl sets this each frame from the
	/// dashboard state so opening the SteamVR rail entry lands the user
	/// straight on the settings/picker without needing the hotkey.
	void SetForceVisible(bool forced);

	/// The helper's own ImGui context. Used by the dashboard input pump
	/// to route SteamVR dashboard mouse events into the right context.
	/// nullptr before Init().
	ImGuiContext* GetContext();
}
