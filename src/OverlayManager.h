// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// IVROverlay-based menu display.
//
// SteamVR's IVROverlay layer system is the supported way to display 2D UI in
// a VR scene from a third-party DLL — the runtime composites the overlay into
// the headset view automatically once we hand it a texture and call
// ShowOverlay. We use BSOpenVR's two-interface pattern:
//
//   * RE::BSOpenVR::GetIVROverlayFromContext(...) — the game's proxied
//     IVROverlay. Use this for everything EXCEPT SetOverlayTexture
//     (CreateOverlay, SetOverlayTransformAbsolute, SetOverlayFlag,
//     SetOverlayWidthInMeters, ShowOverlay/HideOverlay).
//   * RE::BSOpenVR::GetCleanIVROverlay() — a freshly-loaded IVROverlay
//     interface from openvr_api.dll. The game's proxy refuses
//     SetOverlayTexture from foreign DLLs with VROverlayError_PermissionDenied,
//     so we route texture submission through this one.
//
// This module replaces the earlier InSceneOverlay (IVRCompositor::Submit
// hook) approach, which never displayed pixels because we weren't actually
// generating eye-buffer geometry — IVROverlay does the right thing for a
// flat 2D menu.

#pragma once

struct ID3D11Texture2D;

namespace ImGuiVRHelper::OverlayManager
{
	/// Lazy-init the SteamVR overlay handle. Idempotent. Safe to call
	/// every frame — re-runs only until the handle is created. Returns
	/// false if D3D / BSOpenVR / IVROverlay aren't ready yet (caller
	/// should just retry next frame).
	bool EnsureInitialized();

	/// Per-frame submit. Called from HelperImpl::DispatchFrame after the
	/// helper has rendered the focused client's panel and (if dragging)
	/// run the OverlayTinter compute pass.
	///
	/// Looks at `Overlay::State::settings.attachMode` and the focused
	/// client_id to decide whether to show the overlay this frame; calls
	/// ShowOverlay or HideOverlay accordingly.
	void Tick();

	/// Destroy the overlay handle if any. Currently never called — we
	/// rely on process exit to clean up. Exposed so a future plugin
	/// shutdown path can drop the handle cleanly.
	void Shutdown();
}
