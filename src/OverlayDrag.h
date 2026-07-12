// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Grip-to-drag overlay repositioning. Operates on Overlay::State, driven once per
// frame from the helper's Present detour.

#pragma once

namespace ImGuiVRHelper::OverlayDrag
{
	/// Top-level per-frame entry. No-op if drag is disabled or the helper
	/// overlay is not visible. Calls UpdateActiveDrag if a drag is in
	/// progress, otherwise tries to start a new drag.
	void Update();

	/// Returns true if drag input should be sampled at all. False when
	/// drag is disabled in settings, the overlay is hidden, or the OpenVR
	/// runtime is unavailable.
	bool CanPerform();

	/// Recompute the fixed-world transform from the current HMD pose plus
	/// the configured offsets. Called when initializing fixed-world mode
	/// or when the auto-reset distance triggers.
	void SetFixedToCurrentHMD();

	/// Per-frame fixed-world maintenance: lazy initialize on first call,
	/// then auto-reset if the player has wandered past
	/// settings.autoResetDistance from the last anchor.
	void UpdateFixedWorldPositioning();
}
