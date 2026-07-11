// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Runtime-overlay render path: composites the focused panel through the
// runtime's IVROverlay layer system (a SteamVR overlay / an OpenComposite
// OpenXR quad layer) instead of drawing it into the eye buffers, so the menu
// stays sharp under temporal upscalers and frame generation. Opt-in via
// Settings::useRuntimeOverlay; the in-scene path remains the fallback
// whenever the runtime has no usable overlay support.
//
// Threading: which thread performs the IVROverlay calls depends on the
// runtime. SteamVR: input thread only — vrclient calls from the render
// thread race the game's own use (the "device or resource busy" CTD), so
// RenderTick stages a texture copy + transforms and InputTick submits them.
// OpenComposite: render thread only — its SetOverlayTexture copies through
// the caller's D3D11 immediate context, which belongs to the render thread.
// Unknown runtimes get neither; the in-scene path is always safe.

#pragma once

namespace ImGuiVRHelper::RuntimeOverlay
{
	/// Render-thread tick, after HelperImpl::DispatchFrame (needs the
	/// OverlayTinter output to be current). Computes the panel pose and either
	/// submits it directly (OpenComposite) or stages it for InputTick
	/// (SteamVR). Also hides the overlays when the feature is off, nothing is
	/// focused, or the render path got disabled.
	void RenderTick();

	/// Input-thread tick (PollInputDevices thunk). Owns every IVROverlay call
	/// on SteamVR. No-op on other runtimes.
	void InputTick();

	/// True while the runtime is compositing the focused panel, so the
	/// in-scene panel pass stands down (the wand cursor marker still draws —
	/// same anchor math, so it lands on the runtime's quad).
	bool IsHostingPanel();
}
