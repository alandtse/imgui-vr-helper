// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#pragma once

#include "HUDGeometry.h"

#include <DirectXMath.h>

namespace ImGuiVRHelper::InSceneOverlay
{
	inline constexpr float kHUDNearClip = kMinHUDPlaneDepth * 0.5f;
	inline constexpr float kHUDStandardNearClip = 0.1f;
	inline constexpr float kHUDFarClip = 1000.0f;

	/// Planes at or beyond twice the standard near clip keep the standard projection.
	inline bool UsesStandardHUDNearClip(float planeDepth)
	{
		return planeDepth >= kHUDStandardNearClip * 2.0f;
	}

	inline DirectX::XMMATRIX ComputeHUDProjection(float left, float right, float bottom, float top,
		float nearClip = kHUDNearClip)
	{
		return DirectX::XMMatrixPerspectiveOffCenterRH(
			left * nearClip, right * nearClip,
			bottom * nearClip, top * nearClip, nearClip, kHUDFarClip);
	}
}
