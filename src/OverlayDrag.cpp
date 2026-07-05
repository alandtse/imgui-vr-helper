// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Lifted from skyrim-community-shaders/src/Features/VR/OverlayDrag.cpp,
// relicensed GPL-3.0-or-later WITH the modding exception. Adapted to free
// functions in ImGuiVRHelper::OverlayDrag operating on Overlay::State.

#include "pch.h"

#include "OverlayDrag.h"

#include "Overlay.h"
#include "internal/VRUtils.h"

#include <RE/B/BSOpenVR.h>
#include <RE/P/PlayerCharacter.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <vector>

namespace ImGuiVRHelper::OverlayDrag
{
	using DirectX::SimpleMath::Matrix;
	using DirectX::SimpleMath::Vector3;
	using ImGuiVRHelperPluginAPI::InputDeviceType;
	using Overlay::AttachMode;
	using Overlay::DragState;
	using Overlay::PositioningMethod;

	namespace
	{
		bool GetGripPressed(bool isLeft, bool isRight)
		{
			auto& state = Overlay::State::GetSingleton();
			const bool leftHanded = state.lastKnownLeftHandedMode;

			if (isLeft) {
				return leftHanded ? state.primaryControllerState[RE::BSOpenVRControllerDevice::Keys::kGrip].isPressed : state.secondaryControllerState[RE::BSOpenVRControllerDevice::Keys::kGrip].isPressed;
			}
			if (isRight) {
				return leftHanded ? state.secondaryControllerState[RE::BSOpenVRControllerDevice::Keys::kGrip].isPressed : state.primaryControllerState[RE::BSOpenVRControllerDevice::Keys::kGrip].isPressed;
			}
			return false;
		}

		bool CanStartAny(vr::ETrackedControllerRole role)
		{
			return role != vr::TrackedControllerRole_Invalid;
		}

		void UpdateActiveDrag(bool a_clientRequestedThisFrame)
		{
			auto& state = Overlay::State::GetSingleton();
			auto& drag = state.dragState;
			auto& s = state.settings;

			RE::BSOpenVR* openvr = RE::BSOpenVR::GetSingleton();
			auto* system = openvr ? openvr->vrSystem : nullptr;
			if (!system)
				return;

			auto resetDragState = [&]() {
				const bool wasDragging = drag.dragging;
				const auto mode = drag.mode;
				drag.dragging = false;
				drag.controllerIndex = vr::k_unTrackedDeviceIndexInvalid;
				drag.isPrimary = false;
				drag.isSecondary = false;
				drag.clientRequested = false;
				if (!wasDragging)
					return;
				// Fixed-world stores a volatile world matrix that re-anchors as the
				// player moves and re-initializes each session, so persisting it is
				// pointless. Instead capture where the menu ended up as an HMD-relative
				// offset — the same recipe SetFixedToCurrentHMD replays — so the menu
				// respawns where the user put it (next session and on each re-anchor).
				if (mode == DragState::Mode::FixedWorld) {
					vr::TrackedDevicePose_t hmdPose;
					if (!Util::GetDeviceToAbsoluteTrackingPoseCompatible(
							vr::TrackingUniverseStanding, 0, &hmdPose, 1) ||
						!hmdPose.bPoseIsValid) {
						// Couldn't read the HMD pose, so we can't derive the offset.
						// Bail without saving rather than persist a stale one.
						return;
					}
					const Matrix hmd = Util::HmdMatrix34ToMatrix(hmdPose.mDeviceToAbsoluteTracking);
					const Vector3 menuWorld(
						state.fixedWorld.m._41, state.fixedWorld.m._42, state.fixedWorld.m._43);
					const Vector3 local = Vector3::Transform(menuWorld, hmd.Invert());
					s.hmdOffsetX = local.x;
					s.hmdOffsetY = local.y;
					s.hmdOffsetZ = local.z;
				}
				// Persist on release: the drag updates the live settings in real time,
				// but nothing else writes them until the menu closes — so without this
				// a reposition is lost if the user keeps playing (HUD layers never
				// "close") or exits with the menu open.
				Overlay::SaveSettings();
			};

			float rawMatrix[3][4];
			if (Util::GetControllerWorldMatrix(drag.controllerIndex, rawMatrix)) {
				vr::HmdMatrix34_t mat = Util::Float3x4ToHmdMatrix34(rawMatrix);
				Matrix controllerMatrix = Util::HmdMatrix34ToMatrix(mat);

				switch (drag.mode) {
				case DragState::Mode::Controller:
					{
						vr::TrackedDeviceIndex_t attachedIndex = Util::GetControllerIndexForDevice(
							s.attachController, state.lastKnownLeftHandedMode);
						if (attachedIndex != vr::k_unTrackedDeviceIndexInvalid) {
							float attachedM[3][4];
							if (!Util::GetControllerWorldMatrix(attachedIndex, attachedM))
								break;
							Matrix attachedMatrix = Util::HmdMatrix34ToMatrix(Util::Float3x4ToHmdMatrix34(attachedM));

							Vector3 worldDelta(
								controllerMatrix._41 - drag.initialControllerMatrix._41,
								controllerMatrix._42 - drag.initialControllerMatrix._42,
								controllerMatrix._43 - drag.initialControllerMatrix._43);

							Matrix worldToLocal = attachedMatrix.Invert();
							Vector3 localDelta = Vector3::TransformNormal(worldDelta, worldToLocal);

							s.controllerOffsetX = drag.initialControllerOffset.x + localDelta.x;
							s.controllerOffsetY = drag.initialControllerOffset.y + localDelta.y;
							s.controllerOffsetZ = drag.initialControllerOffset.z + localDelta.z;
						}
						break;
					}

				case DragState::Mode::FixedWorld:
					{
						// Rigidly attach the overlay to the controller at grip-press, like
						// picking up a physical object: every frame, recompute the overlay's
						// world pose as if it never left the controller's local frame
						// established at grab time. This gives 1:1 translation AND rotation
						// -- turning the wrist reorients the menu exactly as much as the
						// hand turned, not just where it moved. (HMD/Controller attach modes
						// don't need this: they already recompute from the live attach
						// point's pose every frame, so they always track its orientation.)
						// initialControllerMatrixInverse is cached at grip-press (onInit
						// below) -- it's constant for the drag's lifetime.
						state.fixedWorld.m = drag.initialOverlayMatrix * drag.initialControllerMatrixInverse * controllerMatrix;
						break;
					}

				case DragState::Mode::HMD:
					{
						vr::TrackedDevicePose_t hmdPose;
						if (!Util::GetDeviceToAbsoluteTrackingPoseCompatible(
								vr::TrackingUniverseStanding, 0, &hmdPose, 1)) {
							break;
						}
						if (hmdPose.bPoseIsValid) {
							Matrix hmdMatrix = Util::HmdMatrix34ToMatrix(hmdPose.mDeviceToAbsoluteTracking);

							Vector3 worldDelta(
								controllerMatrix._41 - drag.initialControllerMatrix._41,
								controllerMatrix._42 - drag.initialControllerMatrix._42,
								controllerMatrix._43 - drag.initialControllerMatrix._43);

							Matrix worldToLocal = hmdMatrix.Invert();
							Vector3 localDelta = Vector3::TransformNormal(worldDelta, worldToLocal);

							s.hmdOffsetX = drag.initialHMDOffset.x + localDelta.x;
							s.hmdOffsetY = drag.initialHMDOffset.y + localDelta.y;
							s.hmdOffsetZ = drag.initialHMDOffset.z + localDelta.z;
							s.menuScale = drag.initialHMDScale;
						}
						break;
					}

				default:
					break;
				}
			}

			// Joystick depth control while gripped.
			if (drag.dragging) {
				RE::VRControllerState* gripController = nullptr;
				size_t thumbIdx = 0;
				if (drag.isPrimary) {
					if (state.lastKnownLeftHandedMode) {
						gripController = &state.primaryControllerState;
						thumbIdx = static_cast<size_t>(RE::ControllerRole::Primary);
					} else {
						gripController = &state.secondaryControllerState;
						thumbIdx = static_cast<size_t>(RE::ControllerRole::Secondary);
					}
				} else if (drag.isSecondary) {
					if (state.lastKnownLeftHandedMode) {
						gripController = &state.secondaryControllerState;
						thumbIdx = static_cast<size_t>(RE::ControllerRole::Secondary);
					} else {
						gripController = &state.primaryControllerState;
						thumbIdx = static_cast<size_t>(RE::ControllerRole::Primary);
					}
				}

				if (gripController) {
					const float thumbY = gripController->thumbsticks[thumbIdx].y;
					const float deadzone = s.mouseDeadzone;
					const float depthSpeed = 0.02f;
					if (std::abs(thumbY) > deadzone) {
						const float depthDelta = -thumbY * depthSpeed;
						if (drag.mode == DragState::Mode::HMD) {
							drag.initialHMDOffset.z = std::clamp(
								drag.initialHMDOffset.z + depthDelta, -10.0f, 10.0f);
						} else if (drag.mode == DragState::Mode::Controller) {
							drag.initialControllerOffset.z = std::clamp(
								drag.initialControllerOffset.z + depthDelta, -10.0f, 10.0f);
						} else if (drag.mode == DragState::Mode::FixedWorld) {
							// World-anchored overlay (the DEFAULT positioning mode):
							// push/pull along the line from the HMD to the overlay
							// (thumb up = away). The FixedWorld drag-update re-derives
							// fixedWorld.m from initialOverlayMatrix each frame, so
							// accumulate the depth into that base matrix.
							vr::TrackedDevicePose_t hmdPose;
							if (Util::GetDeviceToAbsoluteTrackingPoseCompatible(
									vr::TrackingUniverseStanding, 0, &hmdPose, 1) &&
								hmdPose.bPoseIsValid) {
								const Matrix hmdMatrix = Util::HmdMatrix34ToMatrix(hmdPose.mDeviceToAbsoluteTracking);
								Vector3 dir(
									drag.initialOverlayMatrix._41 - hmdMatrix._41,
									drag.initialOverlayMatrix._42 - hmdMatrix._42,
									drag.initialOverlayMatrix._43 - hmdMatrix._43);
								if (dir.LengthSquared() > 1e-4f) {
									dir.Normalize();
									const Vector3 move = dir * (-depthDelta);  // thumb up → push away
									drag.initialOverlayMatrix._41 += move.x;
									drag.initialOverlayMatrix._42 += move.y;
									drag.initialOverlayMatrix._43 += move.z;
								}
							}
						}
					}
				}
			}

			// A client-requested drag ends when the client stops calling RequestReposition (its
			// own trigger-held signal) rather than when grip releases -- grip may not even be
			// involved (e.g. it's bound to something else by the underlying game while that
			// client's menu is up), so checking it here would end the drag on the very first
			// frame regardless of what the client wants.
			const bool shouldContinue = drag.clientRequested ?
			                                a_clientRequestedThisFrame :
			                                GetGripPressed(drag.isPrimary, drag.isSecondary);
			if (!shouldContinue) {
				resetDragState();
			}
		}

		void TryStartNewDrag()
		{
			auto& state = Overlay::State::GetSingleton();
			auto& drag = state.dragState;
			auto& s = state.settings;

			RE::BSOpenVR* openvr = RE::BSOpenVR::GetSingleton();
			auto* system = openvr ? openvr->vrSystem : nullptr;
			if (!system)
				return;

			// Off-panel only: while the wand is on the panel, grip is the menu's
			// right-click. Aim off the overlay to grab and reposition it. Keeps
			// grip-to-drag from hijacking a right-click on a menu option.
			if (state.wandState.isIntersecting)
				return;

			struct DragMode
			{
				DragState::Mode mode;
				bool isActive;
				std::function<bool(vr::ETrackedControllerRole)> canStart;
				std::function<void()> onInit;
			};

			std::vector<DragMode> dragModes;

			// Controller mode (highest priority): only the opposite hand.
			if (s.attachMode == AttachMode::ControllerOnly || s.attachMode == AttachMode::Both) {
				dragModes.push_back({ DragState::Mode::Controller, true,
					[&](vr::ETrackedControllerRole role) {
						vr::TrackedDeviceIndex_t attachedIndex = Util::GetControllerIndexForDevice(
							s.attachController, state.lastKnownLeftHandedMode);
						if (attachedIndex == vr::k_unTrackedDeviceIndexInvalid)
							return false;

						InputDeviceType opposite = (s.attachController == InputDeviceType::Primary) ?
					                                   InputDeviceType::Secondary :
					                                   InputDeviceType::Primary;
						vr::TrackedDeviceIndex_t oppositeIndex = Util::GetControllerIndexForDevice(
							opposite, state.lastKnownLeftHandedMode);
						if (oppositeIndex == vr::k_unTrackedDeviceIndexInvalid)
							return false;

						for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i) {
							if (system->GetTrackedDeviceClass(i) == vr::TrackedDeviceClass_Controller) {
								if (system->GetControllerRoleForTrackedDeviceIndex(i) == role && i == oppositeIndex) {
									return true;
								}
							}
						}
						return false;
					},
					[&]() {
						drag.initialControllerOffset.x = s.controllerOffsetX;
						drag.initialControllerOffset.y = s.controllerOffsetY;
						drag.initialControllerOffset.z = s.controllerOffsetZ;
						drag.initialControllerMatrix = drag.startControllerMatrix;
					} });
			}

			// Fixed-world mode.
			if (s.positioningMethod == PositioningMethod::FixedWorld) {
				std::function<bool(vr::ETrackedControllerRole)> canStart;
				if (s.attachMode == AttachMode::Both) {
					canStart = [&](vr::ETrackedControllerRole role) {
						vr::TrackedDeviceIndex_t attachedIndex = Util::GetControllerIndexForDevice(
							s.attachController, state.lastKnownLeftHandedMode);
						if (attachedIndex != vr::k_unTrackedDeviceIndexInvalid) {
							return role == system->GetControllerRoleForTrackedDeviceIndex(attachedIndex);
						}
						return false;
					};
				} else {
					canStart = CanStartAny;
				}

				dragModes.push_back({ DragState::Mode::FixedWorld, true, canStart,
					[&]() {
						drag.initialControllerMatrix = drag.startControllerMatrix;
						drag.initialControllerMatrixInverse = drag.initialControllerMatrix.Invert();
						drag.initialOverlayMatrix = state.fixedWorld.m;
					} });
			}

			// HMD mode.
			if (s.attachMode == AttachMode::HMDOnly || s.attachMode == AttachMode::Both) {
				std::function<bool(vr::ETrackedControllerRole)> canStart;
				if (s.attachMode == AttachMode::Both) {
					canStart = [&](vr::ETrackedControllerRole role) {
						vr::TrackedDeviceIndex_t attachedIndex = Util::GetControllerIndexForDevice(
							s.attachController, state.lastKnownLeftHandedMode);
						if (attachedIndex != vr::k_unTrackedDeviceIndexInvalid) {
							return role == system->GetControllerRoleForTrackedDeviceIndex(attachedIndex);
						}
						return false;
					};
				} else {
					canStart = CanStartAny;
				}

				dragModes.push_back({ DragState::Mode::HMD, true, canStart,
					[&]() {
						drag.initialHMDOffset.x = s.hmdOffsetX;
						drag.initialHMDOffset.y = s.hmdOffsetY;
						drag.initialHMDOffset.z = s.hmdOffsetZ;
						drag.initialHMDScale = s.menuScale;
						drag.initialControllerMatrix = drag.startControllerMatrix;
					} });
			}

			for (const auto& mode : dragModes) {
				if (!mode.isActive)
					continue;
				for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i) {
					if (system->GetTrackedDeviceClass(i) != vr::TrackedDeviceClass_Controller)
						continue;

					vr::ETrackedControllerRole role = system->GetControllerRoleForTrackedDeviceIndex(i);
					const bool isLeft = role == vr::ETrackedControllerRole::TrackedControllerRole_LeftHand;
					const bool isRight = role == vr::ETrackedControllerRole::TrackedControllerRole_RightHand;

					if (!mode.canStart(role))
						continue;
					if (!GetGripPressed(isLeft, isRight))
						continue;

					float rawMatrix[3][4];
					if (!Util::GetControllerWorldMatrix(i, rawMatrix))
						continue;
					Matrix controllerMatrix = Util::HmdMatrix34ToMatrix(Util::Float3x4ToHmdMatrix34(rawMatrix));

					drag.dragging = true;
					drag.mode = mode.mode;
					drag.controllerIndex = i;
					drag.isPrimary = isLeft;
					drag.isSecondary = isRight;
					drag.startControllerMatrix = controllerMatrix;
					mode.onInit();

					// Haptic feedback on grab.
					if (state.overlayVisible && openvr) {
						for (vr::TrackedDeviceIndex_t deviceIdx = 0; deviceIdx < vr::k_unMaxTrackedDeviceCount; ++deviceIdx) {
							if (system->GetTrackedDeviceClass(deviceIdx) != vr::TrackedDeviceClass_Controller)
								continue;
							vr::ETrackedControllerRole deviceRole = system->GetControllerRoleForTrackedDeviceIndex(deviceIdx);
							const bool isRightController = deviceRole == vr::ETrackedControllerRole::TrackedControllerRole_RightHand;
							if (isRightController == isRight) {
								openvr->TriggerHapticPulse(isRightController, 25.0f);
								break;
							}
						}
					}
					return;
				}
			}
		}

		// Client-driven counterpart to TryStartNewDrag: started by HelperImpl::RequestReposition
		// (a client's own on-panel "Move" affordance) instead of an off-panel grip press. Unlike
		// the grip gesture, this is expected to start WHILE the wand is on the panel (that's how
		// the user reached the Move button in the first place) and driven by whichever hand is
		// currently pointing, not a hand search keyed off role/grip.
		void TryStartClientRequestedDrag()
		{
			auto& state = Overlay::State::GetSingleton();
			auto& drag = state.dragState;
			auto& s = state.settings;

			RE::BSOpenVR* openvr = RE::BSOpenVR::GetSingleton();
			auto* system = openvr ? openvr->vrSystem : nullptr;
			if (!system)
				return;

			const vr::TrackedDeviceIndex_t i = state.wandState.controllerIndex;
			if (i == vr::k_unTrackedDeviceIndexInvalid)
				return;

			float rawMatrix[3][4];
			if (!Util::GetControllerWorldMatrix(i, rawMatrix))
				return;
			Matrix controllerMatrix = Util::HmdMatrix34ToMatrix(Util::Float3x4ToHmdMatrix34(rawMatrix));

			vr::ETrackedControllerRole role = system->GetControllerRoleForTrackedDeviceIndex(i);
			const bool isLeft = role == vr::ETrackedControllerRole::TrackedControllerRole_LeftHand;
			const bool isRight = role == vr::ETrackedControllerRole::TrackedControllerRole_RightHand;

			// Same mode selection UpdateActiveDrag's switch expects: FixedWorld takes priority
			// (it's independent of attachMode, and the default positioning method), otherwise
			// whichever attach mode is configured.
			DragState::Mode mode;
			if (s.positioningMethod == PositioningMethod::FixedWorld) {
				mode = DragState::Mode::FixedWorld;
			} else if (s.attachMode == AttachMode::ControllerOnly) {
				mode = DragState::Mode::Controller;
			} else {
				mode = DragState::Mode::HMD;
			}

			drag.dragging = true;
			drag.mode = mode;
			drag.controllerIndex = i;
			drag.isPrimary = isLeft;
			drag.isSecondary = isRight;
			drag.startControllerMatrix = controllerMatrix;
			drag.clientRequested = true;

			switch (mode) {
			case DragState::Mode::FixedWorld:
				drag.initialControllerMatrix = drag.startControllerMatrix;
				drag.initialOverlayMatrix = state.fixedWorld.m;
				break;
			case DragState::Mode::Controller:
				drag.initialControllerOffset = Vector3(s.controllerOffsetX, s.controllerOffsetY, s.controllerOffsetZ);
				drag.initialControllerMatrix = drag.startControllerMatrix;
				break;
			case DragState::Mode::HMD:
				drag.initialHMDOffset = Vector3(s.hmdOffsetX, s.hmdOffsetY, s.hmdOffsetZ);
				drag.initialHMDScale = s.menuScale;
				drag.initialControllerMatrix = drag.startControllerMatrix;
				break;
			default:
				break;
			}

			if (state.overlayVisible) {
				openvr->TriggerHapticPulse(isRight, 25.0f);
			}
		}
	}  // namespace

	bool CanPerform()
	{
		auto& state = Overlay::State::GetSingleton();
		auto& s = state.settings;

		if (!s.enableDragToReposition)
			return false;
		if (!state.overlayVisible)
			return false;

		RE::BSOpenVR* openvr = RE::BSOpenVR::GetSingleton();
		if (!openvr || !openvr->vrSystem)
			return false;

		return true;
	}

	void Update()
	{
		auto& state = Overlay::State::GetSingleton();
		// Consume-once ahead of the CanPerform() gate below: if this frame's request went
		// unconsumed because CanPerform() was false (e.g. overlay briefly not visible), it would
		// otherwise latch true and could start/continue a drag on a LATER frame the client never
		// actually requested for -- the client's heartbeat contract is "this frame", not "since
		// the last time Update() actually ran the rest of this function".
		const bool clientRequestedThisFrame = state.repositionRequested;
		state.repositionRequested = false;

		if (!CanPerform())
			return;

		if (state.dragState.dragging) {
			UpdateActiveDrag(clientRequestedThisFrame);
		} else if (clientRequestedThisFrame) {
			TryStartClientRequestedDrag();
		} else {
			TryStartNewDrag();
		}
	}

	void SetFixedToCurrentHMD()
	{
		auto& state = Overlay::State::GetSingleton();
		auto& s = state.settings;
		vr::HmdMatrix34_t transform = Util::ComputeOverlayTransformFromHMD(
			s.hmdOffsetX, s.hmdOffsetY, s.hmdOffsetZ);
		state.fixedWorld.m = Util::HmdMatrix34ToMatrix(transform);
	}

	void UpdateFixedWorldPositioning()
	{
		auto& state = Overlay::State::GetSingleton();
		auto& s = state.settings;

		if (s.positioningMethod != PositioningMethod::FixedWorld)
			return;

		if (!state.fixedWorld.initialized) {
			state.fixedWorld.initialized = true;
			SetFixedToCurrentHMD();
			if (auto* player = RE::PlayerCharacter::GetSingleton()) {
				state.savedPlayerWorldPos = player->GetPosition();
			}
			return;
		}

		if (s.autoResetDistance > 0.0f) {
			if (auto* player = RE::PlayerCharacter::GetSingleton()) {
				const RE::NiPoint3 playerPos = player->GetPosition();
				const float sqDist = playerPos.GetSquaredDistance(state.savedPlayerWorldPos);
				const float thresholdSq = s.autoResetDistance * s.autoResetDistance;
				if (sqDist > thresholdSq) {
					SetFixedToCurrentHMD();
					state.savedPlayerWorldPos = playerPos;
				}
			}
		}
	}
}
