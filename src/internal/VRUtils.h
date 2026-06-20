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
#include "VRMath.h"              // matrix conversion helpers (re-exported)

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

	// ---- VR property cache ----------------------------------------------
	//
	// IVRSystem *property* queries (eye-to-head, projection, IPD, controller
	// role) lock vrclient's CClientPropertyManager — and the game itself hits
	// that lock from BOTH the input thread (PollInputDevices) and the render
	// thread (MistMenu / BSOpenVR::Unk_0B). There is no safe thread to add them
	// on; safety comes from FREQUENCY. These getters query the runtime rarely
	// (static props once, controller indices at a low rate) and serve cached
	// values, matching open-shaders. GetLastPoses (IVRCompositor, a different
	// lock) remains the only per-frame VR call.

	/// One-time-cached eye-to-head transform. Returns false until the static
	/// cache is populated (needs IVRSystem available). Render-thread safe.
	bool CachedEyeToHead(vr::EVREye eye, vr::HmdMatrix34_t& out);

	/// One-time-cached raw projection tangents. Returns false until populated.
	bool CachedProjectionRaw(vr::EVREye eye, float& left, float& right, float& bottom, float& top);

	/// Map InputDeviceType {Primary,Secondary} to an OpenVR tracked device
	/// index, accounting for the player's handedness. Serves a low-rate-cached
	/// left/right index (a couple of vrclient lookups every ~90 calls).
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

	// Matrix conversion helpers (HmdMatrix34ToMatrix / MatrixToHmdMatrix34 /
	// Float3x4ToHmdMatrix34) live in VRMath.h, included above.

	/// Get the user's IPD (in meters) from the HMD. Tries the direct
	/// property first, then falls back to computing from eye-to-head
	/// transforms, then to the average human IPD (64mm).
	float GetIPDFromHMD();
}
