// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

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

TEST_CASE("plane override clamps to the supported range", "[hud]")
{
	using ImGuiVRHelper::InSceneOverlay::SanitizeHUDPlaneOverride;

	const auto high = SanitizeHUDPlaneOverride(99.0f, 9.0f);
	REQUIRE(high.depth == Approx(5.0f));
	REQUIRE(high.coverage == Approx(1.6f));

	const auto low = SanitizeHUDPlaneOverride(0.01f, 0.01f);
	REQUIRE(low.depth == Approx(0.2f));
	REQUIRE(low.coverage == Approx(0.2f));

	const auto mid = SanitizeHUDPlaneOverride(0.6f, 1.15f);
	REQUIRE(mid.depth == Approx(0.6f));
	REQUIRE(mid.coverage == Approx(1.15f));
}

TEST_CASE("plane override treats non-positive and non-finite as unset", "[hud]")
{
	using ImGuiVRHelper::InSceneOverlay::SanitizeHUDPlaneOverride;

	const auto unset = SanitizeHUDPlaneOverride(0.0f, -1.0f);
	REQUIRE(unset.depth == 0.0f);
	REQUIRE(unset.coverage == 0.0f);

	const float nan = std::numeric_limits<float>::quiet_NaN();
	const auto bad = SanitizeHUDPlaneOverride(nan, std::numeric_limits<float>::infinity());
	REQUIRE(bad.depth == 0.0f);
	REQUIRE(bad.coverage == 0.0f);
}

TEST_CASE("depth convention is resolved from the projection's depth scale", "[hud]")
{
	using ImGuiVRHelper::InSceneOverlay::DepthConvention;
	using ImGuiVRHelper::InSceneOverlay::ResolveDepthConvention;

	// Measured in Skyrim VR: near 13, far 353840, |m10| = 3.67e-5.
	float m[16] = {};
	m[10] = -(13.0f / (353840.0f - 13.0f));
	DepthConvention got = DepthConvention::Standard;
	REQUIRE(ResolveDepthConvention(m, 13.0f, 353840.0f, got));
	REQUIRE(got == DepthConvention::Reversed);

	// The same range under standard depth, where |m10| is ~1.
	m[10] = -(353840.0f / (353840.0f - 13.0f));
	REQUIRE(ResolveDepthConvention(m, 13.0f, 353840.0f, got));
	REQUIRE(got == DepthConvention::Standard);

	// A short range separates the two by only ~1.0001 and must still classify.
	m[10] = -(0.1f / (1000.0f - 0.1f));
	REQUIRE(ResolveDepthConvention(m, 0.1f, 1000.0f, got));
	REQUIRE(got == DepthConvention::Reversed);
	m[10] = -(1000.0f / (1000.0f - 0.1f));
	REQUIRE(ResolveDepthConvention(m, 0.1f, 1000.0f, got));
	REQUIRE(got == DepthConvention::Standard);
}

TEST_CASE("depth convention refuses inputs it cannot classify", "[hud]")
{
	using ImGuiVRHelper::InSceneOverlay::DepthConvention;
	using ImGuiVRHelper::InSceneOverlay::ResolveDepthConvention;

	float m[16] = {};
	DepthConvention got = DepthConvention::Standard;

	// Main menu: infinite far plane, so near and far both read 0.
	REQUIRE_FALSE(ResolveDepthConvention(m, 0.0f, 0.0f, got));

	// A degenerate depth scale cannot be matched to either branch.
	m[10] = 0.0f;
	REQUIRE_FALSE(ResolveDepthConvention(m, 13.0f, 353840.0f, got));
	m[10] = 0.5f;
	REQUIRE_FALSE(ResolveDepthConvention(m, 13.0f, 353840.0f, got));

	const float nan = std::numeric_limits<float>::quiet_NaN();
	m[10] = nan;
	REQUIRE_FALSE(ResolveDepthConvention(m, 13.0f, 353840.0f, got));
	m[10] = -1.0f;
	REQUIRE_FALSE(ResolveDepthConvention(m, nan, 353840.0f, got));
	REQUIRE_FALSE(ResolveDepthConvention(nullptr, 13.0f, 353840.0f, got));
}
