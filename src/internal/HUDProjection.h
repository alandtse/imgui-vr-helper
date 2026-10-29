// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#pragma once

#include "HUDGeometry.h"

#include <DirectXMath.h>

namespace ImGuiVRHelper::InSceneOverlay
{
	inline constexpr float kHUDNearClip = kMinHUDPlaneDepth * 0.5f;
	inline constexpr float kHUDFarClip = 1000.0f;

	inline DirectX::XMMATRIX ComputeHUDProjection(float left, float right, float bottom, float top)
	{
		return DirectX::XMMatrixPerspectiveOffCenterRH(
			left * kHUDNearClip, right * kHUDNearClip,
			bottom * kHUDNearClip, top * kHUDNearClip, kHUDNearClip, kHUDFarClip);
	}
}
