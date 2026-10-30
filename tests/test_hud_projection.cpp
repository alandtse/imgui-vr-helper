// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "internal/HUDProjection.h"

#include <limits>

namespace
{
	DirectX::XMFLOAT4 Project(float x, float y, float depth, DirectX::FXMMATRIX projection)
	{
		DirectX::XMFLOAT4 clip;
		DirectX::XMStoreFloat4(&clip, DirectX::XMVector4Transform(
										  DirectX::XMVectorSet(x, y, -depth, 1.0f), projection));
		return clip;
	}
}

TEST_CASE("positive experimental HUD depths remain distinct and bounded", "[hud]")
{
	using namespace ImGuiVRHelper::InSceneOverlay;
	using Catch::Approx;
	for (const auto depth : { 0.01f, 0.02f, 0.05f, 0.1f, 0.199f, 0.2f, 0.6f, 5.0f }) {
		CAPTURE(depth);
		REQUIRE(SanitizeHUDPlaneOverride(depth, 1.0f).depth == Approx(depth));
	}
	for (const auto depth : { std::numeric_limits<float>::denorm_min(), 0.00001f, 0.009f }) {
		CAPTURE(depth);
		REQUIRE(SanitizeHUDPlaneOverride(depth, 1.0f).depth == Approx(0.01f));
	}
	for (const auto depth : { 0.0f, -0.0f, -0.01f, -1.0f,
			 std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
			 std::numeric_limits<float>::quiet_NaN() }) {
		CAPTURE(depth);
		const auto plane = SanitizeHUDPlaneOverride(depth, 1.15f);
		REQUIRE(plane.depth == 0.0f);
		REQUIRE(plane.coverage == Approx(1.15f));
	}
	const auto plane = SanitizeHUDPlaneOverride(0.01f, 0.0f);
	REQUIRE(plane.depth == Approx(0.01f));
	REQUIRE(plane.coverage == 0.0f);
}

TEST_CASE("HUD projection admits the supported experimental near range", "[hud][projection]")
{
	using namespace ImGuiVRHelper::InSceneOverlay;
	const auto projection = ComputeHUDProjection(-1.1f, 0.9f, -0.8f, 1.2f);
	for (const auto depth : { 0.01f, 0.02f, 0.05f, 0.1f, 0.2f, 0.6f, 5.0f }) {
		CAPTURE(depth);
		const auto clip = Project(0.0f, 0.0f, depth, projection);
		REQUIRE(std::isfinite(clip.z));
		REQUIRE(clip.w > 0.0f);
		REQUIRE(clip.z > 0.0f);
		REQUIRE(clip.z < clip.w);
		REQUIRE(std::abs(clip.x) < clip.w);
		REQUIRE(std::abs(clip.y) < clip.w);
	}
	REQUIRE(Project(0.0f, 0.0f, kHUDNearClip * 0.5f, projection).z < 0.0f);
	const auto oldProjection = DirectX::XMMatrixPerspectiveOffCenterRH(
		-0.11f, 0.09f, -0.08f, 0.12f, 0.1f, 1000.0f);
	REQUIRE(Project(0.0f, 0.0f, 0.01f, oldProjection).z < 0.0f);
}

TEST_CASE("HUD near clip change preserves screen geometry and stereo separation", "[hud][projection]")
{
	using namespace ImGuiVRHelper::InSceneOverlay;
	using Catch::Approx;
	const auto projection = ComputeHUDProjection(-1.1f, 0.9f, -0.8f, 1.2f);
	const auto oldProjection = DirectX::XMMatrixPerspectiveOffCenterRH(
		-0.11f, 0.09f, -0.08f, 0.12f, 0.1f, 1000.0f);
	for (const auto depth : { 0.01f, 0.2f, 0.6f, 5.0f }) {
		for (const auto eyeOffset : { -0.032f, 0.032f }) {
			CAPTURE(depth, eyeOffset);
			const auto clip = Project(eyeOffset, 0.1f * depth, depth, projection);
			const auto oldClip = Project(eyeOffset, 0.1f * depth, depth, oldProjection);
			REQUIRE(clip.x / clip.w == Approx(oldClip.x / oldClip.w));
			REQUIRE(clip.y / clip.w == Approx(oldClip.y / oldClip.w));
		}
	}
	const auto left = Project(0.032f, 0.0f, 0.01f, projection);
	const auto right = Project(-0.032f, 0.0f, 0.01f, projection);
	REQUIRE(left.x / left.w != Approx(right.x / right.w));
}

TEST_CASE("HUD quad corners retain asymmetric stereo coverage", "[hud][projection]")
{
	using namespace ImGuiVRHelper::InSceneOverlay;
	using namespace DirectX;
	using Catch::Approx;
	const float tangents[2][4] = { { -1.2f, 0.8f, -0.9f, 1.1f }, { -0.85f, 1.15f, -1.05f, 0.95f } };
	for (const auto depth : { 0.01f, 0.2f, 0.6f, 5.0f }) {
		for (const auto coverage : { 0.2f, 0.5f, 1.0f, 1.6f }) {
			const auto quad = ComputeHUDQuad(tangents[0], tangents[1], depth, coverage);
			const auto model = XMMatrixScaling(quad.width, quad.height, 1.0f) *
			                   XMMatrixTranslation(0.0f, quad.centerY, -depth);
			for (int eye = 0; eye < 2; ++eye) {
				CAPTURE(depth, coverage, eye);
				const auto& t = tangents[eye];
				const auto view = XMMatrixTranslation(eye == 0 ? 0.032f : -0.032f, 0.0f, 0.0f);
				const auto wvp = model * view * ComputeHUDProjection(t[0], t[1], t[2], t[3]);
				const auto previous = model * view * XMMatrixPerspectiveOffCenterRH(t[0] * 0.1f, t[1] * 0.1f, t[2] * 0.1f, t[3] * 0.1f, 0.1f, 1000.0f);
				for (const auto x : { -0.5f, 0.5f }) {
					for (const auto y : { -0.5f, 0.5f }) {
						XMFLOAT4 clip, oldClip;
						const auto corner = XMVectorSet(x, y, 0.0f, 1.0f);
						XMStoreFloat4(&clip, XMVector4Transform(corner, wvp));
						XMStoreFloat4(&oldClip, XMVector4Transform(corner, previous));
						REQUIRE(clip.z > 0.0f);
						REQUIRE(clip.z < clip.w);
						REQUIRE(clip.x / clip.w == Approx(oldClip.x / oldClip.w).margin(0.00001f));
						REQUIRE(clip.y / clip.w == Approx(oldClip.y / oldClip.w).margin(0.00001f));
					}
				}
			}
		}
	}
}
