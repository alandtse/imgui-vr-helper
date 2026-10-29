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

	/// Per-client HUD plane override, in meters and as a fraction of the view.
	/// A zero field means "use the helper's global setting".
	struct HUDPlaneOverride
	{
		float depth = 0.0f;
		float coverage = 0.0f;
	};

	inline constexpr float kMinHUDPlaneDepth = 0.01f;
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

	inline HUDPlaneOverride ResolveHUDPlane(HUDPlaneOverride plane, HUDPlaneOverride global)
	{
		return { plane.depth > 0.0f ? plane.depth : global.depth,
			plane.coverage > 0.0f ? plane.coverage : global.coverage };
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
