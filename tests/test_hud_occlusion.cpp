// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

#include "internal/HUDGeometry.h"
#include "internal/HUDOcclusion.h"

using namespace ImGuiVRHelper::HUDOcclusion;
using Catch::Approx;

namespace
{
	constexpr float kGameToMetre = 0.01428f;
}

TEST_CASE("Layers are drawn farthest first so the nearest opaque layer wins", "[occlusion][order]")
{
	std::vector<LayerClaim> claims{
		{ 3, 0.5f, 1.0f },
		{ 1, 2.0f, 1.0f },
		{ 2, 1.0f, 1.0f },
	};
	SortFarthestFirst(claims);
	REQUIRE(claims.size() == 3);
	REQUIRE(claims[0].clientId == 1);
	REQUIRE(claims[1].clientId == 2);
	REQUIRE(claims[2].clientId == 3);
}

TEST_CASE("Equal depths keep registration order", "[occlusion][order]")
{
	std::vector<LayerClaim> claims{
		{ 7, 1.0f, 1.0f },
		{ 4, 1.0f, 1.0f },
	};
	SortFarthestFirst(claims);
	REQUIRE(claims[0].clientId == 7);
	REQUIRE(claims[1].clientId == 4);
}

TEST_CASE("HUD plane depth converts to a depth value in either convention", "[occlusion][depth]")
{
	float depth = 0.0f;
	REQUIRE(DepthForMetres(1.0f, 0.1f, 1000.0f, kGameToMetre, DepthConvention::Standard, depth));
	REQUIRE(depth > 0.0f);
	REQUIRE(depth < 1.0f);

	float near = 0.0f, far = 0.0f;
	REQUIRE(DepthForMetres(0.5f, 0.1f, 1000.0f, kGameToMetre, DepthConvention::Standard, near));
	REQUIRE(DepthForMetres(2.0f, 0.1f, 1000.0f, kGameToMetre, DepthConvention::Standard, far));
	REQUIRE(near < far);
}

TEST_CASE("Reversed depth mirrors the standard mapping", "[occlusion][depth][reversed]")
{
	for (const float metres : { 0.5f, 1.0f, 2.0f, 10.0f }) {
		float standard = 0.0f, reversed = 0.0f;
		REQUIRE(DepthForMetres(metres, 0.1f, 1000.0f, kGameToMetre, DepthConvention::Standard, standard));
		REQUIRE(DepthForMetres(metres, 0.1f, 1000.0f, kGameToMetre, DepthConvention::Reversed, reversed));
		REQUIRE(reversed == Approx(1.0f - standard));
	}

	float nearReversed = 0.0f, farReversed = 0.0f;
	REQUIRE(DepthForMetres(0.5f, 0.1f, 1000.0f, kGameToMetre, DepthConvention::Reversed, nearReversed));
	REQUIRE(DepthForMetres(2.0f, 0.1f, 1000.0f, kGameToMetre, DepthConvention::Reversed, farReversed));
	REQUIRE(nearReversed > farReversed);

	REQUIRE(nearReversed > 0.0f);
	REQUIRE(nearReversed < 1.0f);
	float unused = 0.0f;
	REQUIRE_FALSE(DepthForMetres(0.0f, 0.1f, 1000.0f, kGameToMetre, DepthConvention::Reversed, unused));
}

TEST_CASE("An unusable camera or plane writes nothing", "[occlusion][depth]")
{
	float depth = 0.0f;
	REQUIRE_FALSE(DepthForMetres(0.0f, 0.1f, 1000.0f, kGameToMetre, DepthConvention::Standard, depth));
	REQUIRE_FALSE(DepthForMetres(-1.0f, 0.1f, 1000.0f, kGameToMetre, DepthConvention::Standard, depth));
	REQUIRE_FALSE(DepthForMetres(1.0f, 1000.0f, 0.1f, kGameToMetre, DepthConvention::Standard, depth));
	REQUIRE_FALSE(DepthForMetres(1.0f, 5.0f, 5.0f, kGameToMetre, DepthConvention::Standard, depth));
	REQUIRE_FALSE(DepthForMetres(1.0f, 0.0f, 1000.0f, kGameToMetre, DepthConvention::Standard, depth));
	REQUIRE_FALSE(DepthForMetres(1.0f, 0.1f, 1000.0f, 0.0f, DepthConvention::Standard, depth));
	REQUIRE_FALSE(DepthForMetres(1.0f, 0.1f, 1000.0f, -kGameToMetre, DepthConvention::Standard, depth));
	const float nan = std::numeric_limits<float>::quiet_NaN();
	REQUIRE_FALSE(DepthForMetres(nan, 0.1f, 1000.0f, kGameToMetre, DepthConvention::Standard, depth));
	REQUIRE_FALSE(DepthForMetres(1.0f, nan, 1000.0f, kGameToMetre, DepthConvention::Standard, depth));
}

TEST_CASE("Eye viewports split the side-by-side target", "[occlusion][viewport]")
{
	EyeViewport left, right;
	REQUIRE(MakeEyeViewport(3840, 2160, 1.0f, 1.0f, false, left));
	REQUIRE(MakeEyeViewport(3840, 2160, 1.0f, 1.0f, true, right));
	REQUIRE(left.x == Approx(0.0f));
	REQUIRE(left.width == Approx(1920.0f));
	REQUIRE(right.x == Approx(1920.0f));
	REQUIRE(right.width == Approx(1920.0f));
	REQUIRE(left.y == Approx(0.0f));
	REQUIRE(left.height == Approx(2160.0f));
}

TEST_CASE("A fully transparent ratio is rejected rather than producing an empty viewport", "[occlusion][viewport]")
{
	EyeViewport view;
	REQUIRE_FALSE(MakeEyeViewport(3840, 2160, 0.0f, 1.0f, false, view));
	REQUIRE_FALSE(MakeEyeViewport(3840, 2160, 1.0f, 0.0f, false, view));
}

TEST_CASE("Dynamic resolution shrinks the rendered sub-rect", "[occlusion][viewport]")
{
	EyeViewport right;
	REQUIRE(MakeEyeViewport(3840, 2160, 0.5f, 0.5f, true, right));
	REQUIRE(right.x == Approx(1920.0f));
	REQUIRE(right.width == Approx(960.0f));
	REQUIRE(right.height == Approx(1080.0f));
}

TEST_CASE("An unusable target or ratio yields no viewport", "[occlusion][viewport]")
{
	EyeViewport view;
	REQUIRE_FALSE(MakeEyeViewport(1, 2160, 1.0f, 1.0f, false, view));
	REQUIRE_FALSE(MakeEyeViewport(3840, 0, 1.0f, 1.0f, false, view));
	REQUIRE_FALSE(MakeEyeViewport(3840, 2160, 0.0f, 1.0f, false, view));
	REQUIRE_FALSE(MakeEyeViewport(3840, 2160, 1.5f, 1.0f, false, view));
	const float nan = std::numeric_limits<float>::quiet_NaN();
	REQUIRE_FALSE(MakeEyeViewport(3840, 2160, nan, 1.0f, false, view));
}

TEST_CASE("Only Ready enables the depth write", "[occlusion][status]")
{
	REQUIRE(IsReady(BackendStatus::Ready));
	REQUIRE_FALSE(IsReady(BackendStatus::Disabled));
	REQUIRE_FALSE(IsReady(BackendStatus::NoLayers));
	REQUIRE_FALSE(IsReady(BackendStatus::NotMainDepth));
	REQUIRE_FALSE(IsReady(BackendStatus::UnusableDepth));
	REQUIRE_FALSE(IsReady(BackendStatus::NoDevice));
}

TEST_CASE("A plane nearer than the camera near plane clamps or refuses", "[occlusion][depth][near]")
{
	constexpr float kMeasuredNear = 13.0f;
	constexpr float kMeasuredFar = 353840.0f;
	float depth = -1.0f;

	REQUIRE_FALSE(DepthForMetres(0.08f, kMeasuredNear, kMeasuredFar, kGameToMetre,
		DepthConvention::Reversed, depth));
	REQUIRE(DepthForMetres(0.08f, kMeasuredNear, kMeasuredFar, kGameToMetre,
		DepthConvention::Reversed, depth, NearPlanePolicy::Clamp));
	REQUIRE(depth == 1.0f);
	REQUIRE(DepthForMetres(0.08f, kMeasuredNear, kMeasuredFar, kGameToMetre,
		DepthConvention::Standard, depth, NearPlanePolicy::Clamp));
	REQUIRE(depth == 0.0f);

	float unclampedDepth = -1.0f;
	float clampedDepth = -1.0f;
	REQUIRE(DepthForMetres(1.0f, kMeasuredNear, kMeasuredFar, kGameToMetre,
		DepthConvention::Reversed, unclampedDepth));
	REQUIRE(DepthForMetres(1.0f, kMeasuredNear, kMeasuredFar, kGameToMetre,
		DepthConvention::Reversed, clampedDepth, NearPlanePolicy::Clamp));
	REQUIRE(clampedDepth == Approx(unclampedDepth));

	REQUIRE_FALSE(DepthForMetres(0.0f, kMeasuredNear, kMeasuredFar, kGameToMetre,
		DepthConvention::Reversed, depth, NearPlanePolicy::Clamp));
	REQUIRE_FALSE(DepthForMetres(-1.0f, kMeasuredNear, kMeasuredFar, kGameToMetre,
		DepthConvention::Reversed, depth, NearPlanePolicy::Clamp));
	REQUIRE_FALSE(DepthForMetres(1.0f, kMeasuredFar, kMeasuredNear, kGameToMetre,
		DepthConvention::Reversed, depth, NearPlanePolicy::Clamp));
}
