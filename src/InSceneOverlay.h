// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// In-scene overlay rendering: hooks IVRCompositor::Submit and draws the
// focused client's panel texture as a textured quad directly into each
// eye render target.
//
// This is the helper's PRIMARY (and only) rendering path. Universal across
// SteamVR and OpenComposite because every runtime drives its eye textures
// through Submit. SCS uses this approach exclusively; the helper follows
// suit.

#pragma once

#include <openvr.h>

struct ID3D11Texture2D;

namespace ImGuiVRHelper::InSceneOverlay
{
	/// Install the IVRCompositor::Submit detour. Idempotent. Call once
	/// after OpenVR is available (end of InitD3D thunk).
	void Install();

	/// Render the overlay quad into one eye's render target. Called from
	/// the Submit hook for each eye on each frame. `bounds` is the source
	/// rectangle the runtime asked Submit to use (relevant for SBS / texture
	/// arrays); pass nullptr to render to the full target.
	void RenderForEye(vr::EVREye eye, ID3D11Texture2D* targetTexture,
		const vr::VRTextureBounds_t* bounds);
}
