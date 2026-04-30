// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// OpenVR overlay lifecycle. Owns the persistent IVROverlay handle that all
// panel-mode clients share, and submits the focused client's texture to it
// each frame.
//
// HMD-attached only for now; controller-attached and FixedWorld positioning
// land in subsequent commits.

#pragma once

#include <cstdint>

namespace ImGuiVRHelper::OverlayManager
{
	/// Create the IVROverlay handle. Call after the IVR overlay interface
	/// is reachable (i.e. after kDataLoaded, when BSOpenVR is up). Safe to
	/// call multiple times — second call is a no-op.
	void Init();

	/// Submit the focused client's panel texture to the IVROverlay handle
	/// and place it at the configured HMD-relative offset. If no client
	/// is registered, hides the overlay. Called from HelperImpl::DispatchFrame
	/// after per-client OnFrame callbacks complete.
	void SubmitFrame(uint32_t focused_client);

	/// Destroy the overlay handle. Called at SKSE shutdown if/when we wire
	/// up that path. Safe if Init was never called.
	void Shutdown();

	/// True iff Init() succeeded and the overlay is currently shown to the user.
	bool IsVisible();
}
