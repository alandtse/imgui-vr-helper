// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Internal OpenVR utilities. NOT part of the public API.
//
// Lifted from skyrim-community-shaders/src/Utils/VRUtils.{h,cpp} with
// relicensing under GPL-3.0-or-later WITH the modding exception. Drops the
// ImGui-touching helpers (DrawButtonCombo, color constants) — those will
// move into the helper's own settings panel module when that lands.

#pragma once

#include <SimpleMath.h>
#include <openvr.h>

#include "ImGuiVRHelperInput.h"  // InputDeviceType

namespace RE
{
	class BSOpenVR;
}

namespace ImGuiVRHelper::Util
{
	using DirectX::SimpleMath::Matrix;
	using ImGuiVRHelperPluginAPI::InputDeviceType;

	/// Common OpenVR system access pattern. Encapsulates BSOpenVR singleton
	/// retrieval and IVRSystem / IVROverlay interface extraction in one
	/// validation step.
	struct OpenVRContext
	{
		RE::BSOpenVR* openvr = nullptr;
		vr::IVRSystem* system = nullptr;
		vr::IVROverlay* overlay = nullptr;

		OpenVRContext();

		[[nodiscard]] bool IsValid() const { return openvr && system; }
		[[nodiscard]] bool HasOverlay() const { return IsValid() && overlay; }
	};

	/// Compute an overlay transform anchored to the HMD with the given
	/// HMD-local offsets in meters. Returns identity if HMD pose is invalid.
	vr::HmdMatrix34_t ComputeOverlayTransformFromHMD(float offsetX, float offsetY, float offsetZ);

	/// Build a 3x4 transform suitable for SetOverlayTransformTrackedDeviceRelative:
	/// width/height go on the first two diagonal entries, the offset on the
	/// last column. SteamVR places the overlay at this transform in the
	/// parent device's local space. Pass width=height=1.0 if you set the
	/// physical size separately via SetOverlayWidthInMeters.
	vr::HmdMatrix34_t CreateControllerOverlayTransform(
		float offsetX, float offsetY, float offsetZ,
		float width, float height);

	/// Map InputDeviceType {Primary,Secondary} to an OpenVR tracked device
	/// index, accounting for the player's handedness.
	vr::TrackedDeviceIndex_t GetControllerIndexForDevice(InputDeviceType device, bool isLeftHanded);

	/// Get the world matrix for a tracked controller. Returns false if the
	/// pose is invalid or the OpenVR context is unavailable.
	bool GetControllerWorldMatrix(vr::TrackedDeviceIndex_t index, float out[3][4]);

	/// OpenComposite-compatible pose query. Avoids
	/// IVRSystem::GetDeviceToAbsoluteTrackingPose (which has known issues
	/// on OpenComposite) by routing through IVRCompositor::GetLastPoses or
	/// WaitGetPoses.
	bool GetDeviceToAbsoluteTrackingPoseCompatible(
		vr::ETrackingUniverseOrigin eOrigin,
		float fPredictedSecondsToPhotonsFromNow,
		vr::TrackedDevicePose_t* pTrackedDevicePoseArray,
		uint32_t unTrackedDevicePoseArrayCount);

	// ---- Matrix conversion helpers --------------------------------------

	/// Convert an OpenVR HmdMatrix34_t (3x4, column-vector math) to a
	/// DirectX SimpleMath Matrix (4x4, row-vector math).
	inline Matrix HmdMatrix34ToMatrix(const vr::HmdMatrix34_t& m)
	{
		return Matrix(
			m.m[0][0], m.m[1][0], m.m[2][0], 0.0f,
			m.m[0][1], m.m[1][1], m.m[2][1], 0.0f,
			m.m[0][2], m.m[1][2], m.m[2][2], 0.0f,
			m.m[0][3], m.m[1][3], m.m[2][3], 1.0f);
	}

	/// Convert a DirectX SimpleMath Matrix back to an OpenVR HmdMatrix34_t.
	/// Bottom row is discarded.
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

	/// Get the user's IPD (in meters) from the HMD. Tries the direct
	/// property first, then falls back to computing from eye-to-head
	/// transforms, then to the average human IPD (64mm).
	float GetIPDFromHMD();
}
