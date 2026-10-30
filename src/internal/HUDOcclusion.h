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

namespace ImGuiVRHelper::HUDOcclusion
{
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

	/// Which end of the [0,1] depth range the near plane sits at.
	///
	/// Skyrim VR reports reversed depth (near = 1, far = 0), the same convention
	/// HelmetOverlaysNG detects and adapts to rather than refusing to draw. A
	/// hardcoded standard-Z mapping would write the near value for a far plane
	/// and silently hide the world instead of the HUD, so the convention is
	/// resolved from the live projection matrix and passed in.
	enum class DepthConvention
	{
		Standard,  ///< near = 0, far = 1
		Reversed,  ///< near = 1, far = 0
	};

	/// Resolve the convention from the camera projection matrix and the near/far
	/// range the game reports.
	///
	/// The discriminator is the magnitude of the depth scale, m[10]:
	///   standard depth: |m[10]| == far  / (far - near)
	///   reversed depth: |m[10]| == near / (far - near)
	/// Those differ by a factor of far/near, so on a real camera the two are
	/// orders of magnitude apart and never ambiguously close. Measured on Skyrim
	/// VR: near = 13, far = 353840 gives |m[10]| = 3.7e-5, which is the reversed
	/// value (3.674e-5) to 0.7% and is 27000x away from the standard one (1.00003).
	///
	/// The sign of m[10] * perspective is NOT used: it is the same sign for a
	/// mirrored-eye projection and says nothing reliable about the depth range.
	inline bool ResolveDepthConvention(const float* m, float camNear, float camFar,
		DepthConvention& out)
	{
		if (!m || !std::isfinite(camNear) || !std::isfinite(camFar))
			return false;
		if (!(camNear > 0.0f) || !(camFar > camNear))
			return false;
		const float span = camFar - camNear;
		if (!(span > 0.0f))
			return false;
		const float scale = std::abs(m[10]);
		if (!std::isfinite(scale) || !(scale > 0.0f))
			return false;

		const float standard = camFar / span;
		const float reversed = camNear / span;
		// 10% is far tighter than the far/near ratio between the two candidates,
		// so this cannot pick the wrong branch on a real frustum.
		if (std::abs(scale - reversed) <= 0.10f * reversed) {
			out = DepthConvention::Reversed;
			return true;
		}
		if (std::abs(scale - standard) <= 0.10f * standard) {
			out = DepthConvention::Standard;
			return true;
		}
		return false;
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
