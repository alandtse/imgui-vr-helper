// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Lifted from skyrim-community-shaders/src/Features/VR/WandPointing.cpp
// with relicensing under GPL-3.0-or-later WITH the modding exception.
//
// Adapted: pulled out of the SCS VR class into free functions operating on
// Overlay::State::GetSingleton(). UpdateCursorFromWandPointing is dropped —
// its job (driving ImGui IO from the wand) is now per-client work consumed
// via IImGuiVRHelperInterface::GetPointer().

#include "pch.h"

#include "WandPointing.h"

#include "Overlay.h"
#include "internal/VRUtils.h"

#include <cmath>

namespace ImGuiVRHelper::WandPointing
{
	using DirectX::SimpleMath::Matrix;
	using DirectX::SimpleMath::Vector3;

	ImGuiVRHelperPluginAPI::InputDeviceType GetPointingDevice()
	{
		namespace API = ImGuiVRHelperPluginAPI;
		const auto& s = Overlay::State::GetSingleton().settings;
		if (s.attachMode == Overlay::AttachMode::ControllerOnly ||
			s.attachMode == Overlay::AttachMode::Both) {
			return (s.attachController == API::InputDeviceType::Primary) ?
			           API::InputDeviceType::Secondary :
			           API::InputDeviceType::Primary;
		}
		return API::InputDeviceType::Primary;
	}

	bool ComputeIntersectionForOverlayType(Overlay::OverlayType type,
		vr::TrackedDeviceIndex_t controllerIndex, ImVec2& outUV, float& outDepthMeters)
	{
		auto& state = Overlay::State::GetSingleton();
		const auto& s = state.settings;

		float controllerM[3][4];
		if (!Util::GetControllerWorldMatrix(controllerIndex, controllerM)) {
			return false;
		}
		Matrix controllerWorld = Util::HmdMatrix34ToMatrix(Util::Float3x4ToHmdMatrix34(controllerM));
		Vector3 rayOrigin = controllerWorld.Translation();
		Vector3 rayDir = controllerWorld.Forward();

		state.wandState.rayOrigin = rayOrigin;
		state.wandState.rayDirection = rayDir;

		// Poke's touch point: prefer the render model's own "tip" component
		// (vendor-calibrated per controller model -- see CachedControllerTipLocal),
		// falling back to a fixed forward offset from the tracked origin when
		// unavailable (OpenComposite, the headless null-driver harness, or a
		// model with no "tip" component).
		Vector3 tipOrigin;
		vr::HmdMatrix34_t tipLocal;
		if (Util::CachedControllerTipLocal(controllerIndex, tipLocal)) {
			tipOrigin = (Util::HmdMatrix34ToMatrix(tipLocal) * controllerWorld).Translation();
		} else {
			tipOrigin = rayOrigin + rayDir * Overlay::Config::kPokeTipOffsetMeters;
		}

		Matrix overlayWorld;
		if (type == Overlay::OverlayType::HMD) {
			if (s.positioningMethod == Overlay::PositioningMethod::FixedWorld) {
				overlayWorld = state.fixedWorld.m;
			} else {
				vr::TrackedDevicePose_t hmdPose;
				if (!Util::GetDeviceToAbsoluteTrackingPoseCompatible(
						vr::TrackingUniverseStanding, 0, &hmdPose, 1)) {
					return false;
				}
				if (!hmdPose.bPoseIsValid)
					return false;

				Matrix hmdWorld = Util::HmdMatrix34ToMatrix(hmdPose.mDeviceToAbsoluteTracking);
				Matrix offset = Matrix::CreateTranslation(s.hmdOffsetX, s.hmdOffsetY, s.hmdOffsetZ);
				overlayWorld = offset * hmdWorld;
			}
		} else {
			vr::TrackedDeviceIndex_t attachIndex = Util::GetControllerIndexForDevice(
				s.attachController, state.lastKnownLeftHandedMode);
			if (attachIndex == vr::k_unTrackedDeviceIndexInvalid)
				return false;

			float attachM[3][4];
			if (!Util::GetControllerWorldMatrix(attachIndex, attachM))
				return false;

			Matrix attachWorld = Util::HmdMatrix34ToMatrix(Util::Float3x4ToHmdMatrix34(attachM));
			Matrix offset = Matrix::CreateTranslation(
				s.controllerOffsetX, s.controllerOffsetY, s.controllerOffsetZ);
			overlayWorld = offset * attachWorld;
		}

		if (s.menuScale < 1e-4f)
			return false;
		overlayWorld = Overlay::Config::CreateScaleMatrix(s.menuScale) * overlayWorld;

		Matrix worldToOverlay = overlayWorld.Invert();
		Vector3 localOrigin = Vector3::Transform(rayOrigin, worldToOverlay);
		Vector3 localDir = Vector3::TransformNormal(rayDir, worldToOverlay);

		Vector3 localTip = Vector3::Transform(tipOrigin, worldToOverlay);

		// localTip.z is in CreateScaleMatrix-normalized units (the invert
		// above divides out menuScale); undo that for a physical distance.
		outDepthMeters = localTip.z * s.menuScale;

		// Poke: in front of the shell's near boundary, skip the ray and
		// project the tip point straight onto the plane. No lower bound --
		// once past the plane, ANY depth stays in poke mode (there is no
		// physically sensible laser hit "from behind"), so pushing all the
		// way through can't flicker back to the ray-t math below and force a
		// spurious release (confirmed live: it did, right at a symmetric
		// shell's far boundary, before this was one-sided).
		if (outDepthMeters < Overlay::Config::kPokeShellMeters) {
			if (localTip.x < -0.5f || localTip.x > 0.5f ||
				localTip.y < -0.5f || localTip.y > 0.5f)
				return false;
			outUV.x = localTip.x + 0.5f;
			outUV.y = 0.5f - localTip.y;
			return true;
		}

		if (std::abs(localDir.z) < 1e-6f)
			return false;

		float t = -localOrigin.z / localDir.z;
		if (t < 0.0f)
			return false;

		Vector3 hit = localOrigin + t * localDir;

		if (hit.x < -0.5f || hit.x > 0.5f || hit.y < -0.5f || hit.y > 0.5f)
			return false;

		outUV.x = hit.x + 0.5f;
		outUV.y = 0.5f - hit.y;
		return true;
	}

	bool ComputeIntersection(vr::TrackedDeviceIndex_t controllerIndex, ImVec2& outUV)
	{
		auto& state = Overlay::State::GetSingleton();
		const auto attach = state.settings.attachMode;

		bool intersected = false;
		Overlay::OverlayType matchedType = Overlay::OverlayType::HMD;
		float depthMeters = 0.0f;
		if (attach == Overlay::AttachMode::HMDOnly || attach == Overlay::AttachMode::Both) {
			if (ComputeIntersectionForOverlayType(Overlay::OverlayType::HMD, controllerIndex, outUV, depthMeters)) {
				intersected = true;
				matchedType = Overlay::OverlayType::HMD;
			}
		}
		if (!intersected &&
			(attach == Overlay::AttachMode::ControllerOnly || attach == Overlay::AttachMode::Both)) {
			if (ComputeIntersectionForOverlayType(Overlay::OverlayType::Controller, controllerIndex, outUV, depthMeters)) {
				intersected = true;
				matchedType = Overlay::OverlayType::Controller;
			}
		}

		if (intersected) {
			state.wandState.isIntersecting = true;
			state.wandState.uvCoordinatesX.store(outUV.x, std::memory_order_relaxed);
			state.wandState.uvCoordinatesY.store(outUV.y, std::memory_order_relaxed);
			state.wandState.depthMeters.store(depthMeters, std::memory_order_relaxed);
			state.wandState.controllerIndex = controllerIndex;
			state.wandState.matchedOverlayType = matchedType;
		} else {
			state.wandState.isIntersecting = false;
		}
		return intersected;
	}

	void UpdateCursorFromWandPointing()
	{
		auto& state = Overlay::State::GetSingleton();
		const auto& s = state.settings;

		ImGuiIO& io = ImGui::GetIO();

		// Run real raycast first to compute the physical controller's actual
		// intersection state, which is required for off-panel drag-to-reposition logic.
		bool realIntersected = false;
		if (s.enableWandPointing) {
			const auto pointingDevice = GetPointingDevice();

			const auto controllerIndex = Util::GetControllerIndexForDevice(
				pointingDevice, state.lastKnownLeftHandedMode);
			if (controllerIndex != vr::k_unTrackedDeviceIndexInvalid) {
				ImVec2 uv;
				realIntersected = ComputeIntersection(controllerIndex, uv);
				if (realIntersected) {
					float screenX = uv.x * io.DisplaySize.x;
					float screenY = uv.y * io.DisplaySize.y;
					screenX = std::clamp(screenX, 0.0f, io.DisplaySize.x);
					screenY = std::clamp(screenY, 0.0f, io.DisplaySize.y);
					io.MousePos = ImVec2(screenX, screenY);
					io.AddMousePosEvent(screenX, screenY);
					io.WantSetMousePos = false;
				} else {
					state.wandState.isIntersecting = false;
					io.WantSetMousePos = false;
				}
			} else {
				state.wandState.isIntersecting = false;
				io.WantSetMousePos = false;
			}
		} else {
			state.wandState.isIntersecting = false;
			io.WantSetMousePos = false;
		}

		// Synthetic pointer override: drive the cursor from the forced UV,
		// but leave the physical state.wandState.isIntersecting intact to allow
		// off-panel drag testing.
		if (state.debugPointer.active.load(std::memory_order_relaxed)) {
			const float u = std::clamp(state.debugPointer.u.load(std::memory_order_relaxed), 0.0f, 1.0f);
			const float v = std::clamp(state.debugPointer.v.load(std::memory_order_relaxed), 0.0f, 1.0f);
			state.wandState.uvCoordinatesX.store(u, std::memory_order_relaxed);
			state.wandState.uvCoordinatesY.store(v, std::memory_order_relaxed);
			const float x = u * io.DisplaySize.x;
			const float y = v * io.DisplaySize.y;
			io.MousePos = ImVec2(x, y);
			io.AddMousePosEvent(x, y);
			io.WantSetMousePos = false;
		}
	}
}
