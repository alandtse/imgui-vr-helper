// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Pure logic for skipping world rendering behind opaque HUD pixels. No runtime
// dependency — unit-testable headless.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "HUDGeometry.h"

namespace ImGuiVRHelper::HUDOcclusion
{
	using ImGuiVRHelper::InSceneOverlay::DepthConvention;

	/// A HUD layer that will be composited over this frame's eye image, with the
	/// resolved plane it is drawn at. Whether a pixel is actually opaque comes
	/// from that same layer's canvas alpha at draw time, so a claim can never
	/// drift from the art composited on top of the world.
	struct LayerClaim
	{
		uint32_t clientId = 0;
		float depth = 0.0f;
		float coverage = 0.0f;
	};

	/// Farthest plane first. Each layer writes depth only where its own canvas is
	/// opaque, so at any pixel the nearest opaque layer wins and a transparent
	/// layer leaves an earlier, farther claim standing. A pixel covered by any
	/// opaque layer ends up hidden, which is exactly what compositing that layer
	/// over the world does.
	inline void SortFarthestFirst(std::vector<LayerClaim>& claims)
	{
		std::stable_sort(claims.begin(), claims.end(),
			[](const LayerClaim& a, const LayerClaim& b) { return a.depth > b.depth; });
	}

	/// Canvas alpha at or above which the composite replaces the world outright.
	/// Requiring essentially solid alpha keeps blended art (fades, glows,
	/// translucent backdrops) out of occlusion: those pixels still show the world
	/// through them, so the world must keep drawing there.
	inline constexpr float kOpaqueAlphaThreshold = 254.0f / 255.0f;

	inline bool IsOpaque(float alpha)
	{
		return std::isfinite(alpha) && alpha >= kOpaqueAlphaThreshold;
	}

	/// Distance in metres -> game camera units, then to the depth value that
	/// hides the plane under the given convention. Returns false for input that
	/// would write an unusable depth; the caller must then skip the write
	/// instead of guessing.
	inline bool DepthForMetres(float metres, float camNear, float camFar, float gameToMetre,
		DepthConvention convention, float& out)
	{
		if (!(std::isfinite(metres) && std::isfinite(camNear) && std::isfinite(camFar) && std::isfinite(gameToMetre)))
			return false;
		if (!(gameToMetre > 0.0f) || !(camNear > 0.0f) || !(camFar > camNear) || !(metres > 0.0f))
			return false;
		const float gameUnits = metres / gameToMetre;
		if (!(gameUnits > camNear))
			return false;  // at or in front of the near plane: nothing to write
		float depth = (camFar * (gameUnits - camNear)) / (gameUnits * (camFar - camNear));
		if (convention == DepthConvention::Reversed)
			depth = 1.0f - depth;
		if (!(std::isfinite(depth)) || depth < 0.0f || depth > 1.0f)
			return false;
		out = depth;
		return true;
	}

	/// Eye viewport inside the side-by-side depth target. Dynamic resolution
	/// renders a scaled sub-rect of the full-size target, so the eye quad must
	/// not assume the whole half is live. Callers pass the ratios only when the
	/// depth target is still full size; a target that is already shrunk would be
	/// shrunk twice.
	struct EyeViewport
	{
		float x = 0.0f;
		float y = 0.0f;
		float width = 0.0f;
		float height = 0.0f;
	};

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

	/// Why the depth write did not happen. Every one of these renders the world
	/// normally; none of them degrades correctness instead.
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
