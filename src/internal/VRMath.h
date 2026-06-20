// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Pure conversions between OpenVR's HmdMatrix34_t (3x4, column-vector math)
// and DirectX SimpleMath's Matrix (4x4, row-vector math). No runtime
// dependency — unit-testable headless.

#pragma once

#include <SimpleMath.h>
#include <openvr.h>

namespace ImGuiVRHelper::Util
{
	using DirectX::SimpleMath::Matrix;

	/// Convert an OpenVR HmdMatrix34_t to a SimpleMath Matrix (transposed
	/// rotation; translation in the last row).
	inline Matrix HmdMatrix34ToMatrix(const vr::HmdMatrix34_t& m)
	{
		return Matrix(
			m.m[0][0], m.m[1][0], m.m[2][0], 0.0f,
			m.m[0][1], m.m[1][1], m.m[2][1], 0.0f,
			m.m[0][2], m.m[1][2], m.m[2][2], 0.0f,
			m.m[0][3], m.m[1][3], m.m[2][3], 1.0f);
	}

	/// Convert a SimpleMath Matrix back to an OpenVR HmdMatrix34_t. Bottom row
	/// is discarded.
	inline vr::HmdMatrix34_t MatrixToHmdMatrix34(const Matrix& mat)
	{
		vr::HmdMatrix34_t m{};
		m.m[0][0] = mat._11;
		m.m[0][1] = mat._21;
		m.m[0][2] = mat._31;
		m.m[0][3] = mat._41;
		m.m[1][0] = mat._12;
		m.m[1][1] = mat._22;
		m.m[1][2] = mat._32;
		m.m[1][3] = mat._42;
		m.m[2][0] = mat._13;
		m.m[2][1] = mat._23;
		m.m[2][2] = mat._33;
		m.m[2][3] = mat._43;
		return m;
	}

	/// Wrap a raw 3x4 float array into an OpenVR HmdMatrix34_t.
	inline vr::HmdMatrix34_t Float3x4ToHmdMatrix34(const float m[3][4])
	{
		vr::HmdMatrix34_t mat;
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 4; ++j)
				mat.m[i][j] = m[i][j];
		return mat;
	}
}
