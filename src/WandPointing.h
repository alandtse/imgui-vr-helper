// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#pragma once

#include <imgui.h>
#include <openvr.h>

namespace ImGuiVRHelper::Overlay
{
	enum class OverlayType;
}

namespace ImGuiVRHelper::WandPointing
{
	/// Raycast the controller's forward vector against the overlay quad of
	/// the given type (HMD-attached or controller-attached). On hit, fills
	/// outUV with [0..1] panel-local coordinates. Updates the singleton
	/// WandState's debug ray origin/direction unconditionally.
	bool ComputeIntersectionForOverlayType(Overlay::OverlayType type,
		vr::TrackedDeviceIndex_t controllerIndex, ImVec2& outUV);

	/// Try the HMD overlay first (if attached), then the controller overlay
	/// (if attached). Updates singleton WandState with the result.
	bool ComputeIntersection(vr::TrackedDeviceIndex_t controllerIndex, ImVec2& outUV);
}
