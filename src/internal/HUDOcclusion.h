// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "HUDGeometry.h"

namespace ImGuiVRHelper::HUDOcclusion
{
	using ImGuiVRHelper::InSceneOverlay::DepthConvention;

	/// HUD layer identity and resolved plane.
	struct LayerClaim
	{
		uint32_t clientId = 0;
		float depth = 0.0f;
		float coverage = 0.0f;
	};

	/// Sort by descending depth, preserving registration order for ties.
	inline void SortFarthestFirst(std::vector<LayerClaim>& claims)
	{
		std::stable_sort(claims.begin(), claims.end(),
			[](const LayerClaim& a, const LayerClaim& b) { return a.depth > b.depth; });
	}

	/// Minimum sampled canvas alpha eligible for occlusion.
	inline constexpr float kOpaqueAlphaThreshold = 254.0f / 255.0f;

	/// Handling for planes at or inside the camera near plane.
	enum class NearPlanePolicy
	{
		Refuse,  ///< the caller skips this layer
		Clamp,   ///< write the near plane, so the layer culls everything behind it
	};

	/// Convert metres to camera depth; returns false without changing out for unusable inputs.
	inline bool DepthForMetres(float metres, float camNear, float camFar, float gameToMetre,
		DepthConvention convention, float& out, NearPlanePolicy policy = NearPlanePolicy::Refuse)
	{
		if (!(std::isfinite(metres) && std::isfinite(camNear) && std::isfinite(camFar) && std::isfinite(gameToMetre)))
			return false;
		if (!(gameToMetre > 0.0f) || !(camNear > 0.0f) || !(camFar > camNear) || !(metres > 0.0f))
			return false;
		const float gameUnits = metres / gameToMetre;
		if (!(gameUnits > camNear)) {
			if (policy != NearPlanePolicy::Clamp)
				return false;
			out = convention == DepthConvention::Reversed ? 1.0f : 0.0f;
			return true;
		}
		float depth = (camFar * (gameUnits - camNear)) / (gameUnits * (camFar - camNear));
		if (convention == DepthConvention::Reversed)
			depth = 1.0f - depth;
		if (!(std::isfinite(depth)) || depth < 0.0f || depth > 1.0f)
			return false;
		out = depth;
		return true;
	}

	/// Eye viewport inside a side-by-side depth target.
	struct EyeViewport
	{
		float x = 0.0f;
		float y = 0.0f;
		float width = 0.0f;
		float height = 0.0f;
	};

	/// Pass unit ratios when the target is already scaled; returns false for invalid inputs.
	inline bool MakeEyeViewport(uint32_t targetWidth, uint32_t targetHeight,
		float drsWidthRatio, float drsHeightRatio, bool rightEye, EyeViewport& out)
	{
		if (targetWidth < 2 || targetHeight == 0)
			return false;
		if (!(std::isfinite(drsWidthRatio) && std::isfinite(drsHeightRatio)))
			return false;
		if (!(drsWidthRatio > 0.0f && drsWidthRatio <= 1.0f) || !(drsHeightRatio > 0.0f && drsHeightRatio <= 1.0f))
			return false;
		const float eyeWidth = static_cast<float>(targetWidth) * 0.5f;
		out.x = rightEye ? eyeWidth : 0.0f;
		out.y = 0.0f;
		out.width = std::max(1.0f, eyeWidth * drsWidthRatio);
		out.height = std::max(1.0f, static_cast<float>(targetHeight) * drsHeightRatio);
		return true;
	}

	/// Last occlusion attempt's backend status.
	enum class BackendStatus
	{
		Ready,
		Disabled,       ///< user setting off, or the render path is disabled
		NoLayers,       ///< nothing is being composited over the world this frame
		NotMainDepth,   ///< the engine is drawing a shadow map, reflection, etc.
		UnusableDepth,  ///< camera near/far unusable, or the projection is unrecognised
		NoDevice,       ///< D3D not ready, or the kMAIN target is missing
	};

	inline bool IsReady(BackendStatus status)
	{
		return status == BackendStatus::Ready;
	}
}
