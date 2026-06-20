// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "internal/HUDGeometry.h"

using Catch::Approx;
using ImGuiVRHelper::InSceneOverlay::ComputeHUDQuad;
using ImGuiVRHelper::InSceneOverlay::HUDQuad;

TEST_CASE("symmetric frustum fills the FOV at the given depth", "[hud]")
{
	const float eye[4] = { -1.0f, 1.0f, -1.0f, 1.0f };  // ±1 tangents
	const HUDQuad q = ComputeHUDQuad(eye, eye, 2.0f, 1.0f);

	// width = 2 * tanHalfX(=1) * depth(=2) * coverage(=1) = 4
	REQUIRE(q.width == Approx(4.0f));
	REQUIRE(q.height == Approx(4.0f));
	REQUIRE(q.centerY == Approx(0.0f));  // symmetric → centred
}

TEST_CASE("coverage scales extents linearly", "[hud]")
{
	const float eye[4] = { -1.0f, 1.0f, -1.0f, 1.0f };
	const HUDQuad full = ComputeHUDQuad(eye, eye, 1.0f, 1.0f);
	const HUDQuad trim = ComputeHUDQuad(eye, eye, 1.0f, 0.5f);

	REQUIRE(trim.width == Approx(full.width * 0.5f));
	REQUIRE(trim.height == Approx(full.height * 0.5f));
}

TEST_CASE("depth scales extents linearly", "[hud]")
{
	const float eye[4] = { -1.0f, 1.0f, -1.0f, 1.0f };
	const HUDQuad nearQuad = ComputeHUDQuad(eye, eye, 1.0f, 1.0f);
	const HUDQuad farQuad = ComputeHUDQuad(eye, eye, 3.0f, 1.0f);

	REQUIRE(farQuad.width == Approx(nearQuad.width * 3.0f));
	REQUIRE(farQuad.height == Approx(nearQuad.height * 3.0f));
}

TEST_CASE("asymmetric eyes: width covers the larger half-FOV of both", "[hud]")
{
	// Mirror-asymmetric frustums (as real HMDs have): each eye is wider to one
	// side. The shared panel must cover the max half-FOV across both.
	const float left[4] = { -1.2f, 0.8f, -1.0f, 1.0f };
	const float right[4] = { -0.8f, 1.2f, -1.0f, 1.0f };
	const HUDQuad q = ComputeHUDQuad(left, right, 1.0f, 1.0f);

	// max(|−1.2|, 0.8, |−0.8|, 1.2) = 1.2 → width = 2 * 1.2 = 2.4
	REQUIRE(q.width == Approx(2.4f));
}

TEST_CASE("vertically-offset frustum yields a non-zero centerY", "[hud]")
{
	const float eye[4] = { -1.0f, 1.0f, -0.5f, 1.5f };  // top wider than bottom
	const HUDQuad q = ComputeHUDQuad(eye, eye, 1.0f, 1.0f);

	// height = (top + bottom) * depth = (1.5 + 0.5) = 2.0
	// centerY = 0.5 * (top − bottom) * depth = 0.5 * (1.5 − 0.5) = 0.5
	REQUIRE(q.height == Approx(2.0f));
	REQUIRE(q.centerY == Approx(0.5f));
}
