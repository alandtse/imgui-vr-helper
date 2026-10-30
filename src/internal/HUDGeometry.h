// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Pure geometry for the head-locked HUD panel. No runtime dependency —
// unit-testable headless.

#pragma once

#include <algorithm>
#include <cmath>

namespace ImGuiVRHelper::InSceneOverlay
{
	/// Size + vertical centre of the HUD panel in head space (metres).
	struct HUDQuad
	{
		float width = 0.0f;
		float height = 0.0f;
		float centerY = 0.0f;
	};

	/// Which end of [0,1] the near plane sits at in the scene depth buffer.
	enum class DepthConvention
	{
		Standard,  ///< near = 0, far = 1
		Reversed,  ///< near = 1, far = 0
	};

	/// Resolve the convention from a perspective projection and the camera range.
	/// Do not switch to testing the sign of m[10]: a mirrored eye matrix gives the
	/// same sign under either convention. The depth scales differ by far/near.
	/// Returns false rather than guessing when the inputs cannot be classified.
	inline bool ResolveDepthConvention(const float* m, float camNear, float camFar,
		DepthConvention& out)
	{
		if (!m || !std::isfinite(camNear) || !std::isfinite(camFar))
			return false;
		if (!(camNear > 0.0f) || !(camFar > camNear))
			return false;
		const float span = camFar - camNear;
		const float scale = std::abs(m[10]);
		if (!std::isfinite(scale) || !(scale > 0.0f))
			return false;

		constexpr float kTolerance = 0.10f;
		const float reversed = camNear / span;
		if (std::abs(scale - reversed) <= kTolerance * reversed) {
			out = DepthConvention::Reversed;
			return true;
		}
		const float standard = camFar / span;
		if (std::abs(scale - standard) <= kTolerance * standard) {
			out = DepthConvention::Standard;
			return true;
		}
		return false;
	}

	/// Per-client HUD plane override, in meters and as a fraction of the view.
	/// A zero field means "use the helper's global setting".
	struct HUDPlaneOverride
	{
		float depth = 0.0f;
		float coverage = 0.0f;
	};

	inline constexpr float kMinHUDPlaneDepth = 0.2f;
	inline constexpr float kMaxHUDPlaneDepth = 5.0f;
	inline constexpr float kMinHUDPlaneCoverage = 0.2f;
	inline constexpr float kMaxHUDPlaneCoverage = 1.6f;

	/// Clamps a client-supplied plane override; non-finite or non-positive values
	/// reset that field to "use the global setting".
	inline HUDPlaneOverride SanitizeHUDPlaneOverride(float depth, float coverage)
	{
		const auto sanitize = [](float v, float lo, float hi) {
			return (std::isfinite(v) && v > 0.0f) ? std::clamp(v, lo, hi) : 0.0f;
		};
		return { sanitize(depth, kMinHUDPlaneDepth, kMaxHUDPlaneDepth),
			sanitize(coverage, kMinHUDPlaneCoverage, kMaxHUDPlaneCoverage) };
	}

	/// Build an EYE-INDEPENDENT HUD panel so it stereo-converges: a single
	/// head-centred (X=0) quad at `hudDepth`, sized to the larger of the two
	/// eyes' half-FOVs so it still fills the view, with the disparity supplied
	/// per-eye by projection. Sizing/centring per eye instead would cancel the
	/// IPD parallax and pin the panel to infinity.
	///
	/// `projLeft` / `projRight` are each {left, right, bottom, top} raw
	/// projection tangents (left/bottom negative, right/top positive), as
	/// returned by IVRSystem::GetProjectionRaw. `coverage` trims a comfort
	/// margin off the edges.
	inline HUDQuad ComputeHUDQuad(const float projLeft[4], const float projRight[4],
		float hudDepth, float coverage)
	{
		const float lL = projLeft[0], rL = projLeft[1], bL = projLeft[2], tL = projLeft[3];
		const float lR = projRight[0], rR = projRight[1], bR = projRight[2], tR = projRight[3];

		// Half-extent tangents = max magnitude across both eyes (covers both).
		const float tanHalfX = std::max({ -lL, rL, -lR, rR });
		const float tanBottom = std::max(-bL, -bR);
		const float tanTop = std::max(tL, tR);

		HUDQuad quad;
		quad.width = (2.0f * tanHalfX * hudDepth) * coverage;
		quad.height = ((tanTop + tanBottom) * hudDepth) * coverage;
		// Horizontal centre is 0 by construction (both eyes share the panel);
		// vertical frustum centre is usually ~0.
		quad.centerY = 0.5f * (tanTop - tanBottom) * hudDepth;
		return quad;
	}
}
