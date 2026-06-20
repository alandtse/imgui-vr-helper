// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Lifted from skyrim-community-shaders/src/Utils/VRUtils.cpp with
// relicensing under GPL-3.0-or-later WITH the modding exception.

#include "pch.h"

#include "VRUtils.h"

#include <RE/B/BSOpenVR.h>

#include <atomic>
#include <cmath>

namespace ImGuiVRHelper::Util
{
	namespace
	{
		// VR property cache.
		//
		// IVRSystem property reads (eye-to-head, projection, IPD, controller
		// role) lock vrclient's CClientPropertyManager — and the GAME itself
		// touches that lock from BOTH the input thread (PollInputDevices) and
		// the render thread (MistMenu / BSOpenVR::Unk_0B). So there is no "safe
		// thread" to add these calls on; what keeps us safe is FREQUENCY. We
		// query them rarely — static props exactly once, controller indices at a
		// low rate — matching open-shaders (which never CTDs). GetLastPoses
		// (IVRCompositor, a different lock) is the only per-frame VR call.
		struct VRStaticCache
		{
			std::atomic<bool> ready{ false };
			vr::HmdMatrix34_t eyeToHead[2]{};
			float proj[2][4]{};                // [eye] = {left, right, bottom, top}
			std::atomic<float> ipd{ 0.064f };  // average human IPD until populated
		};
		VRStaticCache g_static;

		// Left / right controller device indices, refreshed at a low rate so
		// controller (dis)connect and hand-swap are picked up without per-frame
		// vrclient traffic.
		std::atomic<std::uint32_t> g_leftIndex{ vr::k_unTrackedDeviceIndexInvalid };
		std::atomic<std::uint32_t> g_rightIndex{ vr::k_unTrackedDeviceIndexInvalid };
		std::atomic<int> g_indexRefreshCountdown{ 0 };
		constexpr int kIndexRefreshInterval = 90;  // ~ every 90 lookups

		// IPD from the HMD: direct property, then eye-to-head separation, then
		// the average human IPD (64mm).
		float ComputeIPD(vr::IVRSystem* system)
		{
			vr::ETrackedPropertyError error = vr::TrackedProp_UnknownProperty;
			float ipd = system->GetFloatTrackedDeviceProperty(
				vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_UserIpdMeters_Float, &error);
			if (error == vr::TrackedProp_Success && ipd > 0.0f && ipd < 0.1f)
				return ipd;

			vr::HmdMatrix34_t leftEye = system->GetEyeToHeadTransform(vr::Eye_Left);
			vr::HmdMatrix34_t rightEye = system->GetEyeToHeadTransform(vr::Eye_Right);
			float eyeSeparation = std::abs(leftEye.m[0][3] - rightEye.m[0][3]);
			if (eyeSeparation > 0.0f && eyeSeparation < 0.1f)
				return eyeSeparation;

			return 0.064f;
		}

		// Populate the one-time static cache (a single short burst of IVRSystem
		// reads, like open-shaders' InstallSubmitHook). Returns true once ready.
		bool EnsureStaticCache()
		{
			if (g_static.ready.load(std::memory_order_acquire))
				return true;
			auto* openvr = RE::BSOpenVR::GetSingleton();
			if (!openvr || !openvr->vrSystem)
				return false;
			auto* system = openvr->vrSystem;
			for (int e = 0; e < 2; ++e) {
				const auto eye = static_cast<vr::EVREye>(e);
				g_static.eyeToHead[e] = system->GetEyeToHeadTransform(eye);
				system->GetProjectionRaw(eye, &g_static.proj[e][0], &g_static.proj[e][1],
					&g_static.proj[e][2], &g_static.proj[e][3]);
			}
			g_static.ipd.store(ComputeIPD(system), std::memory_order_relaxed);
			g_static.ready.store(true, std::memory_order_release);
			return true;
		}

		// Refresh the cached left/right indices at most once per
		// kIndexRefreshInterval calls (2 vrclient lookups, low frequency).
		void MaybeRefreshControllerIndices()
		{
			if (g_indexRefreshCountdown.fetch_sub(1, std::memory_order_relaxed) > 0)
				return;
			g_indexRefreshCountdown.store(kIndexRefreshInterval, std::memory_order_relaxed);

			auto* openvr = RE::BSOpenVR::GetSingleton();
			if (!openvr || !openvr->vrSystem)
				return;
			auto* system = openvr->vrSystem;
			g_leftIndex.store(system->GetTrackedDeviceIndexForControllerRole(
								  vr::TrackedControllerRole_LeftHand),
				std::memory_order_relaxed);
			g_rightIndex.store(system->GetTrackedDeviceIndexForControllerRole(
								   vr::TrackedControllerRole_RightHand),
				std::memory_order_relaxed);
		}
	}

	bool CachedEyeToHead(vr::EVREye eye, vr::HmdMatrix34_t& out)
	{
		if (!EnsureStaticCache())
			return false;
		out = g_static.eyeToHead[static_cast<int>(eye)];
		return true;
	}

	bool CachedProjectionRaw(vr::EVREye eye, float& left, float& right, float& bottom, float& top)
	{
		if (!EnsureStaticCache())
			return false;
		const int e = static_cast<int>(eye);
		left = g_static.proj[e][0];
		right = g_static.proj[e][1];
		bottom = g_static.proj[e][2];
		top = g_static.proj[e][3];
		return true;
	}

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
		// Returns the low-rate-cached left/right index (refreshed at most once
		// per kIndexRefreshInterval calls) — at most 2 vrclient lookups every
		// ~90 calls rather than a 64-device scan every frame.
		MaybeRefreshControllerIndices();

		const bool wantLeft = (device == InputDeviceType::Primary) ? isLeftHanded : !isLeftHanded;
		return wantLeft ? g_leftIndex.load(std::memory_order_relaxed) : g_rightIndex.load(std::memory_order_relaxed);
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

		// Only ever read the last submitted pose snapshot. GetLastPoses is safe to
		// call from any thread; WaitGetPoses is the compositor's per-frame sync
		// handshake and must be called by exactly one owner (the game's render
		// loop) once per frame. Calling it from our hooks injects a second runtime
		// handshake into vrclient's IPC, which races the game's input thread and
		// throws std::system_error("device or resource busy"). If GetLastPoses has
		// no frame yet (e.g. early startup), the caller tolerates a missing pose.

		// For single-device requests, use a full pose array internally — keeps
		// OpenComposite happy.
		if (unTrackedDevicePoseArrayCount == 1) {
			vr::TrackedDevicePose_t allPoses[vr::k_unMaxTrackedDeviceCount];
			auto error = compositor->GetLastPoses(allPoses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
			if (error == vr::VRCompositorError_None) {
				pTrackedDevicePoseArray[0] = allPoses[0];
				return true;
			}
			return false;
		}

		auto error = compositor->GetLastPoses(pTrackedDevicePoseArray, unTrackedDevicePoseArrayCount, nullptr, 0);
		return error == vr::VRCompositorError_None;
	}

	float GetIPDFromHMD()
	{
		// One-time cached; defaults to the average human IPD (64mm) until the
		// static cache is populated.
		EnsureStaticCache();
		return g_static.ipd.load(std::memory_order_relaxed);
	}
}
