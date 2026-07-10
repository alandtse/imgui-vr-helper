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
		Overlay::OverlayType matchedType = Overlay::OverlayType::HMD;
		if (attach == Overlay::AttachMode::HMDOnly || attach == Overlay::AttachMode::Both) {
			if (ComputeIntersectionForOverlayType(Overlay::OverlayType::HMD, controllerIndex, outUV)) {
				intersected = true;
				matchedType = Overlay::OverlayType::HMD;
			}
		}
		if (!intersected &&
			(attach == Overlay::AttachMode::ControllerOnly || attach == Overlay::AttachMode::Both)) {
			if (ComputeIntersectionForOverlayType(Overlay::OverlayType::Controller, controllerIndex, outUV)) {
				intersected = true;
				matchedType = Overlay::OverlayType::Controller;
			}
		}

		if (intersected) {
			state.wandState.isIntersecting = true;
			state.wandState.uvCoordinatesX.store(outUV.x, std::memory_order_relaxed);
			state.wandState.uvCoordinatesY.store(outUV.y, std::memory_order_relaxed);
			state.wandState.controllerIndex = controllerIndex;
			state.wandState.matchedOverlayType = matchedType;
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

		ImGuiIO& io = ImGui::GetIO();

		// Synthetic pointer (devbench bridge): drive the cursor from the forced
		// UV so an agent can aim the helper's own UI deterministically, even
		// with no controllers tracked. Precedes every gate on purpose.
		if (state.debugPointer.active.load(std::memory_order_relaxed)) {
			// Clamp to [0,1] before it becomes state.wandState.uvCoordinates: that
			// field is exposed to clients via GetPointer(), which documents (and
			// ComputeIntersectionForOverlayType always maintains) a 0..1 range.
			const float u = std::clamp(state.debugPointer.u.load(std::memory_order_relaxed), 0.0f, 1.0f);
			const float v = std::clamp(state.debugPointer.v.load(std::memory_order_relaxed), 0.0f, 1.0f);
			state.wandState.isIntersecting = true;
			state.wandState.uvCoordinatesX.store(u, std::memory_order_relaxed);
			state.wandState.uvCoordinatesY.store(v, std::memory_order_relaxed);
			const float x = u * io.DisplaySize.x;
			const float y = v * io.DisplaySize.y;
			io.MousePos = ImVec2(x, y);
			io.AddMousePosEvent(x, y);
			// Same no-warp rule as the real wand below: injection runs after the
			// Win32 backend's NewFrame, so it wins without the OS-cursor clamp
			// (WantSetMousePos=true would round-trip through the OS cursor,
			// clamped to the mirror window's client area). MouseDrawCursor is
			// left alone -- SettingsUI's single source of truth decides it from
			// wandState.isIntersecting after this returns.
			io.WantSetMousePos = false;
			return;
		}

		if (!s.enableWandPointing)
			return;

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
			// No WantSetMousePos warp: this runs AFTER the Win32 backend's
			// NewFrame (see SettingsUI), so the injected position wins directly.
			// Round-tripping through the OS cursor clamped the position to the
			// mirror window's client area — a window smaller than the canvas
			// dragged the cursor toward the window edge, diverging from the wand
			// (and the composited dot) worse toward the panel edges.
			io.WantSetMousePos = false;
		} else {
			// Wand off the panel: leave io.MousePos as the Win32 backend's OS
			// mouse sample. Cursor visibility is decided once in
			// SettingsUI::Render after NewFrame (shown at the wand or the mouse,
			// hidden when neither is on the panel), so don't force it here.
			state.wandState.isIntersecting = false;
			io.WantSetMousePos = false;
		}
	}
}
