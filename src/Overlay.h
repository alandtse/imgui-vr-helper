// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Overlay subsystem state: positioning settings, attach modes, wand-laser
// intersection state, and the fixed-world transform fallback.
//
// This is the helper-side replacement for the overlay-related portions of
// SCS's VR class (settings.VRMenu*, wandState, fixedWorldOverlayPosition,
// Config::*). When more PR-2 ports land — overlay submission, drag,
// in-scene fallback — they consume State::GetSingleton().

#pragma once

#include <SimpleMath.h>
#include <imgui.h>
#include <openvr.h>

#include "ImGuiVRHelperInput.h"  // InputDeviceType

namespace ImGuiVRHelper::Overlay
{
	using DirectX::SimpleMath::Matrix;
	using DirectX::SimpleMath::Vector3;
	using ImGuiVRHelperPluginAPI::InputDeviceType;

	// ---- Constants ------------------------------------------------------

	namespace Config
	{
		inline constexpr int kOverlayWidth = 1920;
		inline constexpr int kOverlayHeight = 1080;
		inline constexpr float kOverlayAspect = static_cast<float>(kOverlayHeight) / static_cast<float>(kOverlayWidth);

		inline constexpr float kDefaultMenuScale = 1.0f;
		inline constexpr float kMinMenuScale = 0.1f;
		inline constexpr float kMaxMenuScale = 5.0f;

		// HMD-relative defaults (meters).
		inline constexpr float kDefaultHMDOffsetX = 0.195f;
		inline constexpr float kDefaultHMDOffsetY = -0.375f;
		inline constexpr float kDefaultHMDOffsetZ = -1.355f;

		// Controller-relative defaults (meters).
		inline constexpr float kDefaultControllerOffsetX = 0.295f;
		inline constexpr float kDefaultControllerOffsetY = 0.211f;
		inline constexpr float kDefaultControllerOffsetZ = 0.063f;

		/// Scale matrix matching the overlay's aspect ratio. Y is scaled
		/// proportionally so the overlay stays visually square.
		inline Matrix CreateScaleMatrix(float scale)
		{
			return Matrix::CreateScale(scale, scale * kOverlayAspect, scale);
		}
	}

	// ---- Enums ----------------------------------------------------------

	enum class AttachMode
	{
		HMDOnly = 0,
		ControllerOnly = 1,
		Both = 2,
		None = 3,
	};

	enum class PositioningMethod
	{
		HMDRelative = 0,
		FixedWorld = 1,
	};

	enum class OverlayType
	{
		HMD,
		Controller,
	};

	// ---- Settings -------------------------------------------------------

	struct Settings
	{
		float menuScale = Config::kDefaultMenuScale;
		PositioningMethod positioningMethod = PositioningMethod::FixedWorld;
		AttachMode attachMode = AttachMode::HMDOnly;
		InputDeviceType attachController = InputDeviceType::Secondary;

		// HMD-relative offsets.
		float hmdOffsetX = Config::kDefaultHMDOffsetX;
		float hmdOffsetY = Config::kDefaultHMDOffsetY;
		float hmdOffsetZ = Config::kDefaultHMDOffsetZ;

		// Controller-relative offsets.
		float controllerOffsetX = Config::kDefaultControllerOffsetX;
		float controllerOffsetY = Config::kDefaultControllerOffsetY;
		float controllerOffsetZ = Config::kDefaultControllerOffsetZ;

		bool enableWandPointing = true;

		// Drag-to-reposition.
		bool enableDragToReposition = false;
		float autoResetDistance = 1000.0f;  ///< game units; 0 disables auto-reset
		float mouseDeadzone = 0.1f;         ///< thumbstick deadzone, also used for drag-depth control

		/// RGBA color blended into the overlay while it's being dragged.
		/// Default: yellow at 30% alpha, matching SCS's visual feedback.
		float dragHighlightColor[4] = { 1.0f, 1.0f, 0.0f, 0.3f };
	};

	// ---- Runtime state --------------------------------------------------

	struct WandState
	{
		bool isIntersecting = false;
		ImVec2 uvCoordinates = ImVec2(0.0f, 0.0f);
		vr::TrackedDeviceIndex_t controllerIndex = vr::k_unTrackedDeviceIndexInvalid;
		Vector3 rayOrigin = Vector3::Zero;
		Vector3 rayDirection = Vector3::Zero;
	};

	struct FixedWorldPosition
	{
		Matrix m = Matrix::Identity;
		bool initialized = false;
	};

	struct DragState
	{
		bool dragging = false;
		vr::TrackedDeviceIndex_t controllerIndex = vr::k_unTrackedDeviceIndexInvalid;
		bool isPrimary = false;
		bool isSecondary = false;
		Matrix initialControllerMatrix = Matrix::Identity;
		Matrix initialOverlayMatrix = Matrix::Identity;
		Matrix startControllerMatrix = Matrix::Identity;

		enum class Mode
		{
			None,
			FixedWorld,
			HMD,
			Controller,
		} mode = Mode::None;

		Vector3 initialHMDOffset = Vector3::Zero;
		Vector3 initialControllerOffset = Vector3::Zero;
		float initialHMDScale = 1.0f;
	};

	// ---- Singleton ------------------------------------------------------

	/// Persist Settings to Data/SKSE/Plugins/ImGuiVRHelper.json. Safe to
	/// call any time after Globals::IsReady().
	void SaveSettings();

	/// Load Settings from Data/SKSE/Plugins/ImGuiVRHelper.json if it
	/// exists. Missing fields fall back to defaults; corrupt files log a
	/// warning and leave defaults intact.
	void LoadSettings();

	class State
	{
	public:
		static State& GetSingleton();

		Settings settings;
		WandState wandState;
		FixedWorldPosition fixedWorld;
		DragState dragState;

		bool lastKnownLeftHandedMode = false;
		bool overlayVisible = false;  ///< populated by HelperImpl::IsOverlayVisible

		/// World position the player was at when fixed-world overlay last
		/// snapped. Used by auto-reset distance check.
		RE::NiPoint3 savedPlayerWorldPos{};

		/// Per-controller polled state, populated by Input.cpp once that
		/// port lands. Until then, all isPressed/thumbsticks read 0.
		RE::VRControllerState primaryControllerState{};
		RE::VRControllerState secondaryControllerState{};

	private:
		State() = default;
	};
}
