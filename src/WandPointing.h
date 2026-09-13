// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#pragma once

#include <imgui.h>
#include <openvr.h>

#include "ImGuiVRHelperInput.h"  // InputDeviceType

namespace ImGuiVRHelper::Overlay
{
	enum class OverlayType;
}

namespace ImGuiVRHelper::WandPointing
{
	/// Raycast the controller's forward vector against the overlay quad of
	/// the given type (HMD-attached or controller-attached). On hit, fills
	/// outUV with [0..1] panel-local coordinates and outDepthMeters with the
	/// signed perpendicular distance from the controller to the plane (see
	/// Overlay::Config::kPokeShellMeters). Within that shell the hit is a
	/// direct plane projection (poke) rather than a ray-cast (laser); outside
	/// it, outDepthMeters is still filled in for the caller's bookkeeping even
	/// though the laser math produced the UV. Updates the singleton
	/// WandState's debug ray origin/direction unconditionally.
	bool ComputeIntersectionForOverlayType(Overlay::OverlayType type,
		vr::TrackedDeviceIndex_t controllerIndex, ImVec2& outUV, float& outDepthMeters);

	/// Try the HMD overlay first (if attached), then the controller overlay
	/// (if attached). Updates singleton WandState with the result.
	bool ComputeIntersection(vr::TrackedDeviceIndex_t controllerIndex, ImVec2& outUV);

	/// Which physical controller (Primary/Secondary) does wand pointing/poke
	/// this frame: the opposite hand from attachController when the panel is
	/// controller-attached, otherwise Primary. Single source of truth -- was
	/// previously copy-pasted at each call site (this file and HelperImpl.cpp).
	ImGuiVRHelperPluginAPI::InputDeviceType GetPointingDevice();

	/// Inject the wand-laser hit point into the helper's ImGui context as
	/// a mouse position. Mirrors SCS VR::UpdateCursorFromWandPointing
	/// (origin/open_composite src/Features/VR/WandPointing.cpp:104-145):
	/// pick the OPPOSITE controller from whichever the menu is attached
	/// to, raycast it against the overlay panel, and AddMousePosEvent
	/// with UV*size scaled into pixel coords.
	///
	/// Pre-conditions: helper's settings UI must be visible (caller's
	/// gate) and SettingsUI::Init must have run. No-op otherwise. Sets
	/// io.MouseDrawCursor and io.WantSetMousePos so ImGui draws the
	/// hardware-style cursor at the pointed-at spot.
	void UpdateCursorFromWandPointing();
}
