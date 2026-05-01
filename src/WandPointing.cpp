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

	bool ComputeIntersectionForOverlayType(Overlay::OverlayType type,
		vr::TrackedDeviceIndex_t controllerIndex, ImVec2& outUV)
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
		if (attach == Overlay::AttachMode::HMDOnly || attach == Overlay::AttachMode::Both) {
			if (ComputeIntersectionForOverlayType(Overlay::OverlayType::HMD, controllerIndex, outUV)) {
				intersected = true;
			}
		}
		if (!intersected &&
			(attach == Overlay::AttachMode::ControllerOnly || attach == Overlay::AttachMode::Both)) {
			if (ComputeIntersectionForOverlayType(Overlay::OverlayType::Controller, controllerIndex, outUV)) {
				intersected = true;
			}
		}

		if (intersected) {
			state.wandState.isIntersecting = true;
			state.wandState.uvCoordinates = outUV;
			state.wandState.controllerIndex = controllerIndex;
		} else {
			state.wandState.isIntersecting = false;
		}
		return intersected;
	}

	void UpdateCursorFromWandPointing()
	{
		// One-to-one port of upstream/dev:src/Features/VR/WandPointing.cpp
		// :104-145 (VR::UpdateCursorFromWandPointing). Same control flow,
		// same gating, same MouseDrawCursor / WantSetMousePos handling on
		// both branches. Helper-side adaptation: use Overlay::State /
		// Settings instead of the SCS VR class members.
		auto& state = Overlay::State::GetSingleton();
		const auto& s = state.settings;
		if (!s.enableWandPointing)
			return;

		ImGuiIO& io = ImGui::GetIO();

		namespace API = ImGuiVRHelperPluginAPI;

		// Pointing hand: opposite of menu's attach hand (controller-attached
		// modes), otherwise primary. Matches dev's selector.
		API::InputDeviceType pointingDevice;
		if (s.attachMode == Overlay::AttachMode::ControllerOnly ||
			s.attachMode == Overlay::AttachMode::Both) {
			pointingDevice = (s.attachController == API::InputDeviceType::Primary) ?
			                     API::InputDeviceType::Secondary :
			                     API::InputDeviceType::Primary;
		} else {
			pointingDevice = API::InputDeviceType::Primary;
		}

		const auto controllerIndex = Util::GetControllerIndexForDevice(
			pointingDevice, state.lastKnownLeftHandedMode);
		if (controllerIndex == vr::k_unTrackedDeviceIndexInvalid) {
			state.wandState.isIntersecting = false;
			return;
		}

		ImVec2 uv;
		const bool intersected = ComputeIntersection(controllerIndex, uv);
		if (intersected) {
			float screenX = uv.x * io.DisplaySize.x;
			float screenY = uv.y * io.DisplaySize.y;
			screenX = std::clamp(screenX, 0.0f, io.DisplaySize.x);
			screenY = std::clamp(screenY, 0.0f, io.DisplaySize.y);
			io.MousePos = ImVec2(screenX, screenY);
			io.AddMousePosEvent(screenX, screenY);
			io.MouseDrawCursor = true;
			io.WantSetMousePos = true;
		} else {
			// Deliberate deviation from upstream/dev:WandPointing.cpp:140-145.
			// Dev clears MouseDrawCursor and WantSetMousePos here, which
			// makes the cursor blink off the moment the wand stops
			// intersecting — even if the user had just been driving the
			// cursor with the thumbstick. The user wants the cursor to
			// stay visible at its last position once it's been placed
			// (matches their perception of how SCS behaves in practice).
			// We only clear the intersection bit so combo / GetPointer
			// API consumers see the right state. Cursor visibility is
			// owned by whoever last set it (thumbstick path keeps
			// MouseDrawCursor=true while pushing; this no-op on release
			// preserves that).
			state.wandState.isIntersecting = false;
		}
	}
}
