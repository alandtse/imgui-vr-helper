// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Pure geometry for the head-locked HUD panel. No runtime dependency —
// unit-testable headless.

#pragma once

#include <algorithm>

namespace ImGuiVRHelper::InSceneOverlay
{
	/// Size + vertical centre of the HUD panel in head space (metres).
	struct HUDQuad
	{
		float width = 0.0f;
		float height = 0.0f;
		float centerY = 0.0f;
	};

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
