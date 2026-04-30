// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Compute-shader pass that copies a client's panel texture into a
// helper-owned post-process texture, optionally lerping toward a tint
// color. Runs once per frame after client OnFrame callbacks finish, so
// InSceneOverlay's per-eye composite samples the post-processed result
// instead of the raw panel.
//
// SCS originally did this on the CPU via staging-texture map+blend
// (see Utils/D3D.cpp's ApplyHighlightTintToTexture); their own comment
// flagged "consider compute shader for better performance" because the
// 1920×1080 = 2M pixel pass was slow on the CPU. This is that
// compute-shader version.
//
// Why a separate post-process texture rather than mutating the panel
// in-place: clients clear+redraw their RTV every frame, so any in-place
// tint would survive only briefly and would also confuse clients that
// might read back their own panel. Keeping the panel pristine is the
// less-surprising design.

#pragma once

struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;

namespace ImGuiVRHelper::OverlayTinter
{
	/// Lazy resource init. Returns true once the compute shader, post-process
	/// texture, UAV, SRV, and constant buffer are all created.
	bool EnsureResources();

	/// Copy `srcPanel` into the helper's post-process texture, blending
	/// `tint` (RGBA — alpha is the lerp factor toward tint.rgb). Pass
	/// {0,0,0,0} for "no tint" (the source is copied through unchanged).
	/// Idempotent within a frame; re-dispatches if called multiple times.
	void Dispatch(ID3D11Texture2D* srcPanel, const float tint[4]);

	/// SRV for the post-process texture, suitable as input to
	/// InSceneOverlay's composite. Returns nullptr if EnsureResources
	/// hasn't succeeded yet.
	ID3D11ShaderResourceView* GetOutputSRV();
}
