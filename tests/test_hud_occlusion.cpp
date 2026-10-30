// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Headless tests for the HUD-occlusion decisions: which layers are opaque, the
// order they may write depth in, and the depth value written.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

#include "internal/HUDOcclusion.h"

using namespace ImGuiVRHelper::HUDOcclusion;
using Catch::Approx;

namespace
{
	constexpr float kGameToMetre = 0.01428f;
}

TEST_CASE("Only essentially solid alpha occludes the world", "[occlusion][alpha]")
{
	// Blended art still shows the scene through it, so it must not occlude.
	REQUIRE_FALSE(IsOpaque(0.0f));
	REQUIRE_FALSE(IsOpaque(0.5f));
	REQUIRE_FALSE(IsOpaque(0.98f));
	REQUIRE_FALSE(IsOpaque(253.0f / 255.0f));
	REQUIRE(IsOpaque(1.0f));
	REQUIRE(IsOpaque(254.0f / 255.0f));
}

TEST_CASE("Non-finite alpha never occludes", "[occlusion][alpha]")
{
	REQUIRE_FALSE(IsOpaque(std::numeric_limits<float>::quiet_NaN()));
	REQUIRE_FALSE(IsOpaque(std::numeric_limits<float>::infinity()));
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
	// A 1 m plane with a 0.1 m near and 1000 m far plane sits far from both ends.
	REQUIRE(DepthForMetres(1.0f, 0.1f, 1000.0f, kGameToMetre, DepthConvention::Standard, depth));
	REQUIRE(depth > 0.0f);
	REQUIRE(depth < 1.0f);

	// Closer planes are nearer in depth, farther planes are farther. This is the
	// ordering the depth test relies on.
	float near = 0.0f, far = 0.0f;
	REQUIRE(DepthForMetres(0.5f, 0.1f, 1000.0f, kGameToMetre, DepthConvention::Standard, near));
	REQUIRE(DepthForMetres(2.0f, 0.1f, 1000.0f, kGameToMetre, DepthConvention::Standard, far));
	REQUIRE(near < far);
}

TEST_CASE("Reversed depth mirrors the standard mapping", "[occlusion][depth][reversed]")
{
	// Skyrim VR reports reversed depth (near = 1, far = 0). The mapping must be
	// the mirror of the standard one, not the same numbers, or the stamp would
	// hide the world instead of the HUD.
	// Stays inside the synthetic far plane: 1000 game units x kGameToMetre is
	// ~14.3 m, and a plane beyond it is correctly refused rather than clamped.
	for (const float metres : { 0.5f, 1.0f, 2.0f, 10.0f }) {
		float standard = 0.0f, reversed = 0.0f;
		REQUIRE(DepthForMetres(metres, 0.1f, 1000.0f, kGameToMetre, DepthConvention::Standard, standard));
		REQUIRE(DepthForMetres(metres, 0.1f, 1000.0f, kGameToMetre, DepthConvention::Reversed, reversed));
		REQUIRE(reversed == Approx(1.0f - standard));
	}

	// Ordering must flip with the convention: nearer planes are larger values
	// under reversed depth, which is what makes a "write toward the near plane"
	// cull the world behind the HUD.
	float nearReversed = 0.0f, farReversed = 0.0f;
	REQUIRE(DepthForMetres(0.5f, 0.1f, 1000.0f, kGameToMetre, DepthConvention::Reversed, nearReversed));
	REQUIRE(DepthForMetres(2.0f, 0.1f, 1000.0f, kGameToMetre, DepthConvention::Reversed, farReversed));
	REQUIRE(nearReversed > farReversed);

	// Still bounded, and still refuses a plane at or inside the near plane.
	REQUIRE(nearReversed > 0.0f);
	REQUIRE(nearReversed < 1.0f);
	float unused = 0.0f;
	REQUIRE_FALSE(DepthForMetres(0.0f, 0.1f, 1000.0f, kGameToMetre, DepthConvention::Reversed, unused));
}

TEST_CASE("Depth convention is resolved from the projection's depth scale", "[occlusion][depth][convention]")
{
	// Measured on Skyrim VR: near = 13, far = 353840, |m[10]| = 3.7e-5.
	const float measuredNear = 13.0f, measuredFar = 353840.0f;
	float m[16]{};
	m[10] = -3.7e-5f;  // sign is not the discriminator; magnitude is
	DepthConvention resolved = DepthConvention::Standard;
	REQUIRE(ResolveDepthConvention(m, measuredNear, measuredFar, resolved));
	REQUIRE(resolved == DepthConvention::Reversed);

	// The standard-Z form of the same frustum would be |m[10]| == far/(far-near),
	// which is ~27000x larger here, so the two can never be confused.
	m[10] = measuredFar / (measuredFar - measuredNear);
	REQUIRE(ResolveDepthConvention(m, measuredNear, measuredFar, resolved));
	REQUIRE(resolved == DepthConvention::Standard);

	// A plausible first-person near/far range also resolves, matching HelmetOverlaysNG's
	// finding that VR reports reversed depth.
	const float near = 0.1f, far = 1000.0f;
	m[10] = near / (far - near);
	REQUIRE(ResolveDepthConvention(m, near, far, resolved));
	REQUIRE(resolved == DepthConvention::Reversed);
	m[10] = far / (far - near);
	REQUIRE(ResolveDepthConvention(m, near, far, resolved));
	REQUIRE(resolved == DepthConvention::Standard);

	// Menu/degenerate cameras must be refused, not guessed: at the main menu the
	// game reports an infinite far plane with m[10] ~ 0 and near/far both 0.
	m[10] = -9.8e-5f;
	REQUIRE_FALSE(ResolveDepthConvention(m, 0.0f, 0.0f, resolved));
	m[10] = 0.0f;
	REQUIRE_FALSE(ResolveDepthConvention(m, 0.1f, 1000.0f, resolved));
	// A scale matching neither branch is refused rather than snapped to the near one.
	m[10] = 0.5f;
	REQUIRE_FALSE(ResolveDepthConvention(m, 0.1f, 1000.0f, resolved));
	// Non-finite input.
	m[10] = std::numeric_limits<float>::quiet_NaN();
	REQUIRE_FALSE(ResolveDepthConvention(m, 0.1f, 1000.0f, resolved));
}

TEST_CASE("The production opacity threshold matches the tested constant", "[occlusion][alpha]")
{
	// The pixel shader reads this value from a constant buffer; keeping the
	// boundary case here means a changed threshold cannot silently drift.
	REQUIRE(kOpaqueAlphaThreshold == Approx(254.0f / 255.0f));
	REQUIRE(IsOpaque(kOpaqueAlphaThreshold));
	REQUIRE_FALSE(IsOpaque(std::nextafter(kOpaqueAlphaThreshold, 0.0f)));
}

TEST_CASE("An unusable camera or plane writes nothing", "[occlusion][depth]")
{
	float depth = 0.0f;
	// Behind or exactly at the near plane.
	REQUIRE_FALSE(DepthForMetres(0.0f, 0.1f, 1000.0f, kGameToMetre, DepthConvention::Standard, depth));
	REQUIRE_FALSE(DepthForMetres(-1.0f, 0.1f, 1000.0f, kGameToMetre, DepthConvention::Standard, depth));
	// Inverted or zero-range camera.
	REQUIRE_FALSE(DepthForMetres(1.0f, 1000.0f, 0.1f, kGameToMetre, DepthConvention::Standard, depth));
	REQUIRE_FALSE(DepthForMetres(1.0f, 5.0f, 5.0f, kGameToMetre, DepthConvention::Standard, depth));
	REQUIRE_FALSE(DepthForMetres(1.0f, 0.0f, 1000.0f, kGameToMetre, DepthConvention::Standard, depth));
	// Degenerate unit scale.
	REQUIRE_FALSE(DepthForMetres(1.0f, 0.1f, 1000.0f, 0.0f, DepthConvention::Standard, depth));
	REQUIRE_FALSE(DepthForMetres(1.0f, 0.1f, 1000.0f, -kGameToMetre, DepthConvention::Standard, depth));
	// Non-finite inputs.
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
