// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Lifted from skyrim-community-shaders/src/Utils/VRUtils.cpp with
// relicensing under GPL-3.0-or-later WITH the modding exception.

#include "pch.h"

#include "VRUtils.h"

#include <RE/B/BSOpenVR.h>

namespace ImGuiVRHelper::Util
{
	OpenVRContext::OpenVRContext()
	{
		openvr = RE::BSOpenVR::GetSingleton();
		if (openvr) {
			system = openvr->vrSystem;
			overlay = RE::BSOpenVR::GetIVROverlayFromContext(&openvr->vrContext);
		}
	}

	vr::HmdMatrix34_t ComputeOverlayTransformFromHMD(float offsetX, float offsetY, float offsetZ)
	{
		// Identity, used as the early-return fallback so callers never see
		// uninitialized matrix elements.
		vr::HmdMatrix34_t transform = {};
		transform.m[0][0] = 1.0f;
		transform.m[1][1] = 1.0f;
		transform.m[2][2] = 1.0f;

		auto* openvr = RE::BSOpenVR::GetSingleton();
		if (!openvr)
			return transform;

		auto* system = openvr->vrSystem;
		if (!system)
			return transform;

		vr::TrackedDevicePose_t hmdPose;
		if (!GetDeviceToAbsoluteTrackingPoseCompatible(
				vr::TrackingUniverseStanding, 0, &hmdPose, 1)) {
			return transform;
		}
		if (!hmdPose.bPoseIsValid)
			return transform;

		transform = hmdPose.mDeviceToAbsoluteTracking;

		// Apply offsets in HMD local space.
		transform.m[0][3] += transform.m[0][0] * offsetX + transform.m[0][1] * offsetY + transform.m[0][2] * offsetZ;
		transform.m[1][3] += transform.m[1][0] * offsetX + transform.m[1][1] * offsetY + transform.m[1][2] * offsetZ;
		transform.m[2][3] += transform.m[2][0] * offsetX + transform.m[2][1] * offsetY + transform.m[2][2] * offsetZ;

		return transform;
	}

	bool FillEyeViewProjMatrices(vr::EVREye eye, float outView[16], float outProj[16])
	{
		using DirectX::SimpleMath::Matrix;

		auto* openvr = RE::BSOpenVR::GetSingleton();
		if (!openvr || !openvr->vrSystem)
			return false;

		vr::TrackedDevicePose_t hmdPose;
		if (!GetDeviceToAbsoluteTrackingPoseCompatible(
				vr::TrackingUniverseStanding, 0, &hmdPose, 1)) {
			return false;
		}
		if (!hmdPose.bPoseIsValid)
			return false;

		// View: world-space -> eye-space. eyeToWorld = eyeToHead * hmdWorld
		// (DirectX row-vector convention). View = inverse(eyeToWorld).
		const Matrix eyeToHead = HmdMatrix34ToMatrix(
			openvr->vrSystem->GetEyeToHeadTransform(eye));
		const Matrix hmdWorld = HmdMatrix34ToMatrix(hmdPose.mDeviceToAbsoluteTracking);
		const Matrix view = (eyeToHead * hmdWorld).Invert();

		// Projection from raw left/right/bottom/top tangents. Note Valve's
		// known parameter-name mismatch in GetProjectionRaw — the 3rd
		// param is bottom, the 4th is top. Use named locals matching
		// the actual values.
		float left, right, bottom, top;
		openvr->vrSystem->GetProjectionRaw(eye, &left, &right, &bottom, &top);
		constexpr float nearZ = 0.1f;
		constexpr float farZ = 1000.0f;
		const Matrix proj = DirectX::XMMatrixPerspectiveOffCenterRH(
			left * nearZ, right * nearZ, bottom * nearZ, top * nearZ, nearZ, farZ);

		// SimpleMath::Matrix derives from XMFLOAT4X4: 16 floats laid out
		// row-major (_11, _12, _13, _14, _21, ...). Same memory layout
		// as the float[16] the API specifies, so memcpy is exact.
		std::memcpy(outView, &view, sizeof(float) * 16);
		std::memcpy(outProj, &proj, sizeof(float) * 16);
		return true;
	}

	vr::HmdMatrix34_t CreateControllerOverlayTransform(
		float offsetX, float offsetY, float offsetZ,
		float width, float height)
	{
		// Same shape SCS uses (Utils/VRUtils.cpp:CreateControllerOverlayTransform):
		// width on m[0][0], height on m[1][1], identity Z, offset on the
		// last column. SteamVR multiplies the result against the parent
		// device pose internally.
		vr::HmdMatrix34_t transform{};
		transform.m[0][0] = width;
		transform.m[0][3] = offsetX;
		transform.m[1][1] = height;
		transform.m[1][3] = offsetY;
		transform.m[2][2] = 1.0f;
		transform.m[2][3] = offsetZ;
		return transform;
	}

	vr::TrackedDeviceIndex_t GetControllerIndexForDevice(InputDeviceType device, bool isLeftHanded)
	{
		OpenVRContext ctx;
		if (!ctx.IsValid())
			return vr::k_unTrackedDeviceIndexInvalid;

		vr::ETrackedControllerRole targetRole;
		if (device == InputDeviceType::Primary) {
			targetRole = isLeftHanded ? vr::ETrackedControllerRole::TrackedControllerRole_LeftHand : vr::ETrackedControllerRole::TrackedControllerRole_RightHand;
		} else {
			targetRole = isLeftHanded ? vr::ETrackedControllerRole::TrackedControllerRole_RightHand : vr::ETrackedControllerRole::TrackedControllerRole_LeftHand;
		}

		for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i) {
			if (ctx.system->GetTrackedDeviceClass(i) == vr::TrackedDeviceClass_Controller) {
				if (ctx.system->GetControllerRoleForTrackedDeviceIndex(i) == targetRole) {
					return i;
				}
			}
		}
		return vr::k_unTrackedDeviceIndexInvalid;
	}

	bool GetControllerWorldMatrix(vr::TrackedDeviceIndex_t index, float out[3][4])
	{
		OpenVRContext ctx;
		if (!ctx.IsValid())
			return false;

		vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
		if (!GetDeviceToAbsoluteTrackingPoseCompatible(
				vr::TrackingUniverseStanding, 0, poses, vr::k_unMaxTrackedDeviceCount)) {
			return false;
		}
		if (!poses[index].bPoseIsValid)
			return false;

		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 4; ++j)
				out[i][j] = poses[index].mDeviceToAbsoluteTracking.m[i][j];
		return true;
	}

	bool GetDeviceToAbsoluteTrackingPoseCompatible(
		vr::ETrackingUniverseOrigin /*eOrigin*/,
		float /*fPredictedSecondsToPhotonsFromNow*/,
		vr::TrackedDevicePose_t* pTrackedDevicePoseArray,
		uint32_t unTrackedDevicePoseArrayCount)
	{
		OpenVRContext ctx;
		if (!ctx.IsValid())
			return false;

		auto* compositor = RE::BSOpenVR::GetIVRCompositor();
		if (!compositor && ctx.openvr) {
			compositor = ctx.openvr->vrContext.vrCompositor;
		}
		if (!compositor)
			return false;

		// For single-device requests, use a full pose array internally — keeps
		// OpenComposite happy.
		if (unTrackedDevicePoseArrayCount == 1) {
			vr::TrackedDevicePose_t allPoses[vr::k_unMaxTrackedDeviceCount];
			auto error = compositor->GetLastPoses(allPoses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
			if (error == vr::VRCompositorError_None) {
				pTrackedDevicePoseArray[0] = allPoses[0];
				return true;
			}
			error = compositor->WaitGetPoses(allPoses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
			if (error == vr::VRCompositorError_None) {
				pTrackedDevicePoseArray[0] = allPoses[0];
				return true;
			}
			return false;
		}

		auto error = compositor->GetLastPoses(pTrackedDevicePoseArray, unTrackedDevicePoseArrayCount, nullptr, 0);
		if (error == vr::VRCompositorError_None)
			return true;

		error = compositor->WaitGetPoses(pTrackedDevicePoseArray, unTrackedDevicePoseArrayCount, nullptr, 0);
		return error == vr::VRCompositorError_None;
	}

	float GetIPDFromHMD()
	{
		auto* openvr = RE::BSOpenVR::GetSingleton();
		if (!openvr || !openvr->vrSystem)
			return 0.064f;  // average human IPD fallback

		// Method 1: query property directly.
		vr::ETrackedPropertyError error = vr::TrackedProp_UnknownProperty;
		float ipd = openvr->vrSystem->GetFloatTrackedDeviceProperty(
			vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_UserIpdMeters_Float, &error);
		if (error == vr::TrackedProp_Success && ipd > 0.0f && ipd < 0.1f) {
			return ipd;
		}

		// Method 2: derive from eye-to-head transforms.
		vr::HmdMatrix34_t leftEye = openvr->vrSystem->GetEyeToHeadTransform(vr::Eye_Left);
		vr::HmdMatrix34_t rightEye = openvr->vrSystem->GetEyeToHeadTransform(vr::Eye_Right);
		float eyeSeparation = std::abs(leftEye.m[0][3] - rightEye.m[0][3]);
		if (eyeSeparation > 0.0f && eyeSeparation < 0.1f) {
			return eyeSeparation;
		}

		return 0.064f;
	}
}
