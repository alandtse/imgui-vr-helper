// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Net-new helper code (not a 1:1 port of SCS's Input.cpp). The shape of
// the controller state population is similar, but the helper's role here
// is to build wire-shaped Frame snapshots for delivery to client mods —
// it never drives ImGui IO directly.
//
// SCS's Input.cpp behavior that intentionally lives elsewhere:
//   - Menu open/close combo matching      → HelperImpl::ComboFired
//   - ImGui::GetIO() mouse/wheel/key pokes → per-client OnFrame consumer
//   - Wand-pointer cursor synthesis       → IImGuiVRHelperInterface::GetPointer
//
// The button mapping table here is structured around SCS's existing
// keybind conventions (kGrip/kGripAlt → GripClick, kTouchpadClick/Alt →
// PadClick, etc.) since SCS will be the first client.

#include "pch.h"

#include "Input.h"

#include "Overlay.h"
#include "internal/VRUtils.h"

#include <RE/B/BSOpenVR.h>
#include <RE/B/BSOpenVRControllerDevice.h>

#include <chrono>
#include <deque>
#include <mutex>

namespace ImGuiVRHelper::Input
{
	namespace API = ImGuiVRHelperPluginAPI;
	using Keys = RE::BSOpenVRControllerDevice::Keys;

	std::span<const ButtonInfo> ButtonTable()
	{
		// reKey, wireButton, canonicalKey, name, candidate, diagnostics.
		// Alternates (Oculus axis-2 grip/touchpad) fold onto their base key.
		static constexpr ButtonInfo kButtons[] = {
			{ Keys::kBY, API::Button::BY, Keys::kBY, "B/Y", true, true },
			{ Keys::kGrip, API::Button::GripClick, Keys::kGrip, "Grip", true, true },
			{ Keys::kGripAlt, API::Button::GripClick, Keys::kGrip, "GripAlt", true, true },
			{ Keys::kXA, API::Button::AX, Keys::kXA, "A/X", true, true },
			{ Keys::kJoystickTrigger, API::Button::StickClick, Keys::kJoystickTrigger, "Stick Click", true, true },
			{ Keys::kTrigger, API::Button::TriggerClick, Keys::kTrigger, "Trigger", true, true },
			{ Keys::kTouchpadClick, API::Button::PadClick, Keys::kTouchpadClick, "Touchpad", true, true },
			{ Keys::kTouchpadAlt, API::Button::PadClick, Keys::kTouchpadClick, "Touchpad Alt", true, false },
		};
		return kButtons;
	}

	const char* ButtonName(uint32_t keyCode)
	{
		for (const auto& b : ButtonTable())
			if (b.reKey == keyCode)
				return b.name;
		return "?";
	}

	uint32_t Canonical(uint32_t keyCode)
	{
		for (const auto& b : ButtonTable())
			if (b.reKey == keyCode)
				return b.canonicalKey;
		return keyCode;
	}

	namespace
	{
		// ---- State diff for edge detection ------------------------------

		struct EdgeState
		{
			uint32_t prev_held_left = 0;
			uint32_t prev_held_right = 0;
		};

		EdgeState g_edges;

		// ---- Handedness tracking ----------------------------------------

		// Protect handedness plus controller snapshots across threads.
		std::mutex g_controllerStateMutex;

		bool g_lastKnownLeftHanded = false;
		bool g_handednessInitialized = false;

		// Caller must hold g_controllerStateMutex.
		void RefreshHandednessLocked()
		{
			const bool now = RE::BSOpenVRControllerDevice::IsLeftHandedMode();
			if (!g_handednessInitialized || now != g_lastKnownLeftHanded) {
				if (g_handednessInitialized && now != g_lastKnownLeftHanded) {
					logs::info("VR handedness changed: {} -> {}",
						g_lastKnownLeftHanded ? "Left" : "Right",
						now ? "Left" : "Right");
					auto& state = Overlay::State::GetSingleton();
					state.primaryControllerState = {};
					state.secondaryControllerState = {};
				}
				g_handednessInitialized = true;
				g_lastKnownLeftHanded = now;
				Overlay::State::GetSingleton().lastKnownLeftHandedMode = now;
			}
		}

		uint32_t ButtonsToWireMask(const RE::VRControllerState& state)
		{
			uint32_t mask = 0;
			for (const auto& m : ButtonTable()) {
				if (state[m.reKey].isPressed) {
					mask |= 1u << static_cast<uint32_t>(m.wireButton);
				}
			}
			return mask;
		}

		// Recent VR controller events ring buffer (diagnostics UI). Written
		// from FeedVREvent (input thread), read via SnapshotEventLog (render
		// thread) — guarded by a mutex.
		constexpr size_t kMaxEventLog = 32;
		std::mutex g_eventLogMutex;
		std::deque<EventLogEntry> g_eventLog;

		// Synthetic button events (devbench bridge), queued from devbench's
		// listener thread and drained on the input thread so they land in
		// controller state with the same threading as real events.
		struct InjectedEvent
		{
			bool primaryHand;
			uint32_t keyCode;
			bool pressed;
		};
		std::mutex g_injectMutex;
		std::vector<InjectedEvent> g_injectQueue;

		bool IsButtonKey(uint32_t keyCode)
		{
			for (const auto& m : ButtonTable()) {
				if (m.reKey == keyCode)
					return true;
			}
			return false;
		}

		// ---- Pose population --------------------------------------------

		void FillPoseFromHmdMatrix(API::Pose& dst, const vr::HmdMatrix34_t& m)
		{
			// Translation (rightmost column of the 3x4).
			dst.pos[0] = m.m[0][3];
			dst.pos[1] = m.m[1][3];
			dst.pos[2] = m.m[2][3];

			// Convert the 3x3 rotation portion to a quaternion.
			const float trace = m.m[0][0] + m.m[1][1] + m.m[2][2];
			float qw, qx, qy, qz;
			if (trace > 0.0f) {
				const float s = 0.5f / std::sqrt(trace + 1.0f);
				qw = 0.25f / s;
				qx = (m.m[2][1] - m.m[1][2]) * s;
				qy = (m.m[0][2] - m.m[2][0]) * s;
				qz = (m.m[1][0] - m.m[0][1]) * s;
			} else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
				const float s = 2.0f * std::sqrt(1.0f + m.m[0][0] - m.m[1][1] - m.m[2][2]);
				qw = (m.m[2][1] - m.m[1][2]) / s;
				qx = 0.25f * s;
				qy = (m.m[0][1] + m.m[1][0]) / s;
				qz = (m.m[0][2] + m.m[2][0]) / s;
			} else if (m.m[1][1] > m.m[2][2]) {
				const float s = 2.0f * std::sqrt(1.0f + m.m[1][1] - m.m[0][0] - m.m[2][2]);
				qw = (m.m[0][2] - m.m[2][0]) / s;
				qx = (m.m[0][1] + m.m[1][0]) / s;
				qy = 0.25f * s;
				qz = (m.m[1][2] + m.m[2][1]) / s;
			} else {
				const float s = 2.0f * std::sqrt(1.0f + m.m[2][2] - m.m[0][0] - m.m[1][1]);
				qw = (m.m[1][0] - m.m[0][1]) / s;
				qx = (m.m[0][2] + m.m[2][0]) / s;
				qy = (m.m[1][2] + m.m[2][1]) / s;
				qz = 0.25f * s;
			}
			dst.orient[0] = qw;
			dst.orient[1] = qx;
			dst.orient[2] = qy;
			dst.orient[3] = qz;

			dst.vel[0] = dst.vel[1] = dst.vel[2] = 0.0f;
			dst.angvel[0] = dst.angvel[1] = dst.angvel[2] = 0.0f;
			dst.valid = 1u | 2u;  // tracked + visible; velocity not populated yet
		}

		bool TryFillHmdPose(API::Pose& out)
		{
			vr::TrackedDevicePose_t pose;
			if (!Util::GetDeviceToAbsoluteTrackingPoseCompatible(
					vr::TrackingUniverseStanding, 0, &pose, 1)) {
				return false;
			}
			if (!pose.bPoseIsValid)
				return false;
			FillPoseFromHmdMatrix(out, pose.mDeviceToAbsoluteTracking);
			return true;
		}

		bool TryFillControllerPose(API::Pose& out, vr::TrackedDeviceIndex_t idx)
		{
			if (idx == vr::k_unTrackedDeviceIndexInvalid)
				return false;
			float m[3][4];
			if (!Util::GetControllerWorldMatrix(idx, m))
				return false;
			FillPoseFromHmdMatrix(out, Util::Float3x4ToHmdMatrix34(m));
			return true;
		}

		// ---- Hand population --------------------------------------------

		void FillHand(API::Hand& dst, const RE::VRControllerState& state,
			vr::TrackedDeviceIndex_t deviceIdx, size_t thumbRoleIdx)
		{
			dst = {};
			dst.connected = (deviceIdx != vr::k_unTrackedDeviceIndexInvalid) ? 1u : 0u;
			dst.controller_kind = 0;  // unknown — extend when we read controller manufacturer string

			if (dst.connected) {
				TryFillControllerPose(dst.pose, deviceIdx);
			} else {
				dst.pose = {};
			}

			dst.buttons_held = ButtonsToWireMask(state);

			// Trigger and grip analog: SCS doesn't expose continuous values
			// in RE::VRControllerState, so synthesize 0/1 from press state.
			dst.trigger = state[Keys::kTrigger].isPressed ? 1.0f : 0.0f;
			dst.grip = (state[Keys::kGrip].isPressed || state[Keys::kGripAlt].isPressed) ? 1.0f : 0.0f;

			dst.stick_x = state.thumbsticks[thumbRoleIdx].x;
			dst.stick_y = state.thumbsticks[thumbRoleIdx].y;
			dst.pad_x = 0.0f;
			dst.pad_y = 0.0f;

			// haptic_token: pack tracked device index so TriggerHaptic can
			// route back to this controller. Reserve 0 for "no haptic".
			dst.haptic_token = dst.connected ? (deviceIdx + 1u) : 0u;
		}
	}  // namespace

	void FeedVREvent(uint32_t device, uint32_t keyCode, bool pressed,
		float thumbstickX, float thumbstickY)
	{
		const auto deviceEnum = static_cast<RE::INPUT_DEVICE>(device);
		const bool isPrimary = RE::BSOpenVRControllerDevice::IsPrimaryController(deviceEnum);
		const bool isSecondary = RE::BSOpenVRControllerDevice::IsSecondaryController(deviceEnum);
		if (!isPrimary && !isSecondary)
			return;

		// Button event: update isPressed via OnEvent for the matching key.
		const auto nowSecs = std::chrono::duration<double>(
			std::chrono::steady_clock::now().time_since_epoch())
		                         .count();

		const size_t thumbIdx = static_cast<size_t>(
			isPrimary ? RE::ControllerRole::Primary : RE::ControllerRole::Secondary);
		constexpr float kHardwareDriftFloor = 0.05f;
		const float snappedX = (std::abs(thumbstickX) < kHardwareDriftFloor) ? 0.0f : thumbstickX;
		const float snappedY = (std::abs(thumbstickY) < kHardwareDriftFloor) ? 0.0f : thumbstickY;

		{
			std::scoped_lock lk{ g_controllerStateMutex };
			RefreshHandednessLocked();

			auto& state = Overlay::State::GetSingleton();
			auto& target = isPrimary ? state.primaryControllerState : state.secondaryControllerState;

			// Store the canonical key so one physical button maps to one entry.
			for (const auto& m : ButtonTable()) {
				if (keyCode == m.reKey) {
					target[m.canonicalKey].OnEvent(pressed, nowSecs);
					break;
				}
			}

			target.thumbsticks[thumbIdx].x = snappedX;
			target.thumbsticks[thumbIdx].y = snappedY;
		}

		// Record into the diagnostics event log. Classify as a thumbstick
		// event when the key isn't one of the mapped buttons.
		{
			const bool isStick = !IsButtonKey(keyCode);
			std::scoped_lock lk{ g_eventLogMutex };
			g_eventLog.push_back(EventLogEntry{ device, keyCode, pressed, isStick, snappedX, snappedY });
			if (g_eventLog.size() > kMaxEventLog)
				g_eventLog.pop_front();
		}
	}

	void InvalidateHandedness()
	{
		std::scoped_lock lk{ g_controllerStateMutex };
		g_handednessInitialized = false;
	}

	bool IsLeftHanded()
	{
		std::scoped_lock lk{ g_controllerStateMutex };
		return g_lastKnownLeftHanded;
	}

	void ResetEdgeState()
	{
		g_edges = {};
	}

	std::vector<EventLogEntry> SnapshotEventLog()
	{
		std::scoped_lock lk{ g_eventLogMutex };
		return std::vector<EventLogEntry>(g_eventLog.begin(), g_eventLog.end());
	}

	void BuildFrame(API::Frame& out, float dt)
	{
		out = {};
		out.abi_version = API::kInputAbiVersion;
		out.struct_size = sizeof(API::Frame);
		out.dt = dt;

		// HMD pose.
		if (!TryFillHmdPose(out.hmd)) {
			out.hmd.valid = 0;
		}

		bool leftHanded;
		RE::VRControllerState primarySnapshot;
		RE::VRControllerState secondarySnapshot;
		{
			std::scoped_lock lk{ g_controllerStateMutex };
			RefreshHandednessLocked();
			auto& state = Overlay::State::GetSingleton();
			leftHanded = g_lastKnownLeftHanded;
			primarySnapshot = state.primaryControllerState;
			secondarySnapshot = state.secondaryControllerState;
		}

		auto& state = Overlay::State::GetSingleton();
		const auto primaryIdx = Util::GetControllerIndexForDevice(
			ImGuiVRHelperPluginAPI::InputDeviceType::Primary, leftHanded);
		const auto secondaryIdx = Util::GetControllerIndexForDevice(
			ImGuiVRHelperPluginAPI::InputDeviceType::Secondary, leftHanded);
		const size_t primaryThumb = static_cast<size_t>(RE::ControllerRole::Primary);
		const size_t secondaryThumb = static_cast<size_t>(RE::ControllerRole::Secondary);

		if (leftHanded) {
			FillHand(out.left, primarySnapshot, primaryIdx, primaryThumb);
			FillHand(out.right, secondarySnapshot, secondaryIdx, secondaryThumb);
		} else {
			FillHand(out.right, primarySnapshot, primaryIdx, primaryThumb);
			FillHand(out.left, secondarySnapshot, secondaryIdx, secondaryThumb);
		}

		// Edge detection: compute pressed/released by diffing against the
		// previous frame's held bitmask.
		const uint32_t leftHeld = out.left.buttons_held;
		const uint32_t rightHeld = out.right.buttons_held;

		out.left.buttons_pressed = leftHeld & ~g_edges.prev_held_left;
		out.left.buttons_released = ~leftHeld & g_edges.prev_held_left;
		out.right.buttons_pressed = rightHeld & ~g_edges.prev_held_right;
		out.right.buttons_released = ~rightHeld & g_edges.prev_held_right;

		g_edges.prev_held_left = leftHeld;
		g_edges.prev_held_right = rightHeld;

		// Helper-side flags are filled in by HelperImpl when it dispatches
		// to clients (focus, overlay visibility, pointer-in-panel). Leave
		// flags at 0 here; HelperImpl::DispatchFrame patches them per-client.
		out.flags = state.overlayVisible ? (1u << 1) : 0u;
	}

	void InjectButton(bool primaryHand, uint32_t keyCode, bool pressed)
	{
		std::scoped_lock lk(g_injectMutex);
		// Drains every input poll (well under a second even at the lowest realistic
		// poll rate), so a legitimate caller never gets near this; it only bites a
		// caller injecting far faster than the game polls input.
		constexpr size_t kMaxQueuedInjections = 256;
		if (g_injectQueue.size() >= kMaxQueuedInjections) {
			logs::warn("InjectButton: queue full ({} pending), dropping event", kMaxQueuedInjections);
			return;
		}
		g_injectQueue.push_back({ primaryHand, keyCode, pressed });
	}

	void DrainInjected()
	{
		std::vector<InjectedEvent> pending;
		{
			std::scoped_lock lk(g_injectMutex);
			if (g_injectQueue.empty())
				return;
			pending.swap(g_injectQueue);
		}
		auto& state = Overlay::State::GetSingleton();
		const auto nowSecs = std::chrono::duration<double>(
			std::chrono::steady_clock::now().time_since_epoch())
		                         .count();
		std::scoped_lock lk{ g_controllerStateMutex };
		for (const auto& e : pending) {
			auto& target = e.primaryHand ? state.primaryControllerState :
			                               state.secondaryControllerState;
			// Same canonical fold real events get, so one synthetic button is
			// exactly one state entry (see FeedVREvent).
			target[Canonical(e.keyCode)].OnEvent(e.pressed, nowSecs);
		}
	}
}
