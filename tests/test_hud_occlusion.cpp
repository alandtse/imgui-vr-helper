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

TEST_CASE("HUD plane depth converts to a standard-Z depth value", "[occlusion][depth]")
{
	float depth = 0.0f;
	// A 1 m plane with a 0.1 m near and 1000 m far plane sits far from both ends.
	REQUIRE(StandardZDepth(1.0f, 0.1f, 1000.0f, kGameToMetre, depth));
	REQUIRE(depth > 0.0f);
	REQUIRE(depth < 1.0f);

	// Closer planes are nearer in depth, farther planes are farther. This is the
	// ordering the depth test relies on.
	float near = 0.0f, far = 0.0f;
	REQUIRE(StandardZDepth(0.5f, 0.1f, 1000.0f, kGameToMetre, near));
	REQUIRE(StandardZDepth(2.0f, 0.1f, 1000.0f, kGameToMetre, far));
	REQUIRE(near < far);
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
	REQUIRE_FALSE(StandardZDepth(0.0f, 0.1f, 1000.0f, kGameToMetre, depth));
	REQUIRE_FALSE(StandardZDepth(-1.0f, 0.1f, 1000.0f, kGameToMetre, depth));
	// Inverted or zero-range camera.
	REQUIRE_FALSE(StandardZDepth(1.0f, 1000.0f, 0.1f, kGameToMetre, depth));
	REQUIRE_FALSE(StandardZDepth(1.0f, 5.0f, 5.0f, kGameToMetre, depth));
	REQUIRE_FALSE(StandardZDepth(1.0f, 0.0f, 1000.0f, kGameToMetre, depth));
	// Degenerate unit scale.
	REQUIRE_FALSE(StandardZDepth(1.0f, 0.1f, 1000.0f, 0.0f, depth));
	REQUIRE_FALSE(StandardZDepth(1.0f, 0.1f, 1000.0f, -kGameToMetre, depth));
	// Non-finite inputs.
	const float nan = std::numeric_limits<float>::quiet_NaN();
	REQUIRE_FALSE(StandardZDepth(nan, 0.1f, 1000.0f, kGameToMetre, depth));
	REQUIRE_FALSE(StandardZDepth(1.0f, nan, 1000.0f, kGameToMetre, depth));
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
	REQUIRE_FALSE(IsReady(BackendStatus::RendererConflict));
	REQUIRE_FALSE(IsReady(BackendStatus::NotMainDepth));
	REQUIRE_FALSE(IsReady(BackendStatus::UnusableDepth));
	REQUIRE_FALSE(IsReady(BackendStatus::NoDevice));
}
