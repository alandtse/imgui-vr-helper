// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Depth-only world occlusion behind fully opaque HUD pixels.
//
// Pixels the HUD covers completely are invisible behind it, so rasterising the
// world there is wasted work. Before the game issues its first world batch this
// stamps the HUD plane's depth into kMAIN wherever the composited layer's own
// canvas alpha is opaque, so the depth test discards the world behind the panel
// while anything nearer still draws.
//
// Opacity comes from the layer's own canvas, never from a separate client mask,
// so the write cannot drift from the art composited on top of it. Colour and
// stencil are never written. Every situation this code cannot prove safe renders
// the world normally instead of guessing.

#pragma once

#include <cstdint>

namespace ImGuiVRHelper::HUDOcclusion
{
	enum class BackendStatus;  // internal/HUDOcclusion.h

	/// Install the world-start hook. Call once the render path is up. Idempotent,
	/// non-throwing, and VR-only. The hook is always installed; whether a frame
	/// actually stamps is decided per frame by the safety checks, so a renderer
	/// mod that loads later still turns the stamp off.
	void Install();

	/// Why the last frame did or did not stamp depth. Diagnostics only.
	BackendStatus LastStatus();

	/// One-line status for the settings UI and logs.
	const char* StatusText();
}
