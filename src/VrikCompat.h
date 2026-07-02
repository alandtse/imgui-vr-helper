// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#pragma once

namespace ImGuiVRHelper::VrikCompat
{
	// One-time VRIK interface handshake (safe no-op if VRIK isn't installed). Call once, after
	// other plugins have finished loading (kPostPostLoad or later) so the dispatch reaches VRIK
	// regardless of plugin load order.
	void TryFetchInterface();

	// VRIK's current per-frame head-bob + height-smoothing camera offset (Skyrim world units,
	// Z-only in practice), or RE::NiPoint3::Zero() if VRIK isn't installed/fetched. VRIK applies
	// this to its camera node's local Z each frame to simulate footstep sway; it has no
	// counterpart in the real OpenVR HMD pose, so anything computed from real tracking (like our
	// world-quad billboards) needs to apply the same offset to stay visually aligned with
	// Skyrim's own (VRIK-shifted) scene render.
	RE::NiPoint3 GetCameraOffset();
}
