// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "internal/VRMath.h"

using Catch::Approx;
using ImGuiVRHelper::Util::Float3x4ToHmdMatrix34;
using ImGuiVRHelper::Util::HmdMatrix34ToMatrix;
using ImGuiVRHelper::Util::Matrix;
using ImGuiVRHelper::Util::MatrixToHmdMatrix34;

namespace
{
	vr::HmdMatrix34_t MakeHmd(const float (&v)[3][4])
	{
		vr::HmdMatrix34_t m{};
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 4; ++j)
				m.m[i][j] = v[i][j];
		return m;
	}
}

TEST_CASE("HmdMatrix34 <-> Matrix is a lossless round-trip", "[vrmath]")
{
	const float v[3][4] = { { 1, 2, 3, 4 }, { 5, 6, 7, 8 }, { 9, 10, 11, 12 } };
	const vr::HmdMatrix34_t src = MakeHmd(v);

	const vr::HmdMatrix34_t back = MatrixToHmdMatrix34(HmdMatrix34ToMatrix(src));

	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 4; ++j)
			REQUIRE(back.m[i][j] == Approx(src.m[i][j]));
}

TEST_CASE("HmdMatrix34ToMatrix puts translation in the last row", "[vrmath]")
{
	const float v[3][4] = { { 1, 0, 0, 1.5f }, { 0, 1, 0, -2.5f }, { 0, 0, 1, 3.5f } };
	const Matrix m = HmdMatrix34ToMatrix(MakeHmd(v));

	REQUIRE(m._41 == Approx(1.5f));
	REQUIRE(m._42 == Approx(-2.5f));
	REQUIRE(m._43 == Approx(3.5f));
	REQUIRE(m._44 == Approx(1.0f));
}

TEST_CASE("Float3x4ToHmdMatrix34 copies every element", "[vrmath]")
{
	const float in[3][4] = { { 1, 2, 3, 4 }, { 5, 6, 7, 8 }, { 9, 10, 11, 12 } };
	const vr::HmdMatrix34_t out = Float3x4ToHmdMatrix34(in);

	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 4; ++j)
			REQUIRE(out.m[i][j] == Approx(in[i][j]));
}
