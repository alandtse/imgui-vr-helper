// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2025 ImGuiVRHelper contributors. See api/COPYING.LESSER.
//
// Wire-stable data structures for the ImGuiVRHelper public API.
//
// IMPORTANT: this header must NOT include ImGui or OpenVR headers, must NOT
// reference any ImGuiKey/IVR* type, and must NOT change layout once
// shipped. Add new fields by bumping kInputAbiVersion and extending the
// trailing portion of structs, never by reordering existing fields.

#pragma once

#include <cstddef>
#include <cstdint>

struct ID3D11RenderTargetView;

namespace ImGuiVRHelperPluginAPI
{
	inline constexpr uint32_t kInputAbiVersion = 1;

	/// 3D pose in OpenVR standing-universe space, meters and quaternions.
	struct Pose
	{
		float pos[3];     ///< meters; x right, y up, z toward user
		float orient[4];  ///< quaternion w, x, y, z
		float vel[3];
		float angvel[3];
		uint32_t valid;  ///< bit0 tracked, bit1 visible, bit2 has_velocity
	};

	/// Wire-stable button identifiers. Owned by the helper; never renumbered.
	/// The helper translates from RE::BSOpenVRControllerDevice::Keys at the
	/// boundary so this enum is independent of the OpenVR/SkyrimVR mapping.
	enum class Button : uint32_t
	{
		AX = 0,            ///< RE Keys::kXA              (7)  — X (left) / A (right) face button
		BY = 1,            ///< RE Keys::kBY              (1)  — Y (left) / B (right) face button
		Menu = 2,          ///< system menu / app menu   (varies by controller)
		System = 3,        ///< system / home button     (varies)
		TriggerClick = 4,  ///< RE Keys::kTrigger         (33) — analog trigger pulled past click threshold
		GripClick = 5,     ///< RE Keys::kGrip            (2)  — grip pressed; kGripAlt (34) folded in
		StickClick = 6,    ///< RE Keys::kJoystickTrigger (32) — thumbstick pressed in
		PadClick = 7,      ///< RE Keys::kTouchpadClick   (35) — touchpad clicked; kTouchpadAlt (36) folded in
		Shoulder = 8,      ///< reserved for future controllers with shoulder buttons
		Reserved9 = 9,
		Reserved10 = 10,
		Reserved11 = 11,
		Reserved12 = 12,
		Reserved13 = 13,
		Reserved14 = 14,
		Reserved15 = 15,
	};

	/// Per-controller per-frame state.
	struct Hand
	{
		uint32_t connected;
		uint32_t controller_kind;  ///< 0 unknown, 1 index, 2 oculus_touch, 3 wmr, 4 vive, 5 cosmos, ...
		Pose pose;
		uint32_t buttons_held;      ///< bitmask: (1u << static_cast<uint32_t>(Button::X))
		uint32_t buttons_pressed;   ///< edges this frame
		uint32_t buttons_released;  ///< edges this frame
		uint32_t buttons_touched;   ///< capacitive
		float trigger;              ///< 0..1 analog
		float grip;                 ///< 0..1 analog
		float stick_x, stick_y;     ///< -1..1
		float pad_x, pad_y;         ///< -1..1
		uint32_t haptic_token;      ///< opaque; pass back to TriggerHaptic()
	};

	/// Per-frame snapshot delivered to each registered client via OnFrameFn.
	struct Frame
	{
		uint32_t abi_version;  ///< == kInputAbiVersion
		uint32_t struct_size;  ///< == sizeof(Frame); allows forward-extension
		float dt;              ///< seconds since previous frame
		Pose hmd;
		Hand left;
		Hand right;
		uint32_t flags;  ///< bit0 client_has_focus
						 ///< bit1 overlay_visible
						 ///< bit2 client_pointer_in_panel
	};

	/// Helper-owned render target the client renders its ImGui frame into.
	/// Valid until UnregisterClient. Width/height may change between frames
	/// if the user resizes the panel; clients should re-check each frame.
	struct PanelHandle
	{
		uint32_t width;
		uint32_t height;
		ID3D11RenderTargetView* rtv;
	};

	using ComboId = uint32_t;

	/// Forward declaration; defined in ImGuiVRHelperInput.h.
	struct InputCombo;

	using OnFrameFn = void (*)(const Frame*, void* user);
	using ComboRecordedFn = void (*)(const InputCombo*, std::size_t n, void* user);

	/// Bit flags for RegisterClient().
	enum ClientFlags : uint32_t
	{
		kClientFlag_None = 0,
		kClientFlag_RequiresFocus = 1u << 0,  ///< only invoke on_frame when this client holds focus

		/// Render this client's panel as a fully transparent, screen-space
		/// overlay covering the entire eye viewport in BOTH eyes — instead
		/// of as a 3D quad floating in front of the user.
		///
		/// Use cases: subtitle text positioned over an NPC's head, damage
		/// numbers, world-locked 2D HUD elements where the client
		/// computes screen-space coordinates from world-space positions.
		///
		/// Behavior:
		///   - The panel RTV is the same 1920×1080 RGBA8 the helper hands
		///     to all clients. The client should clear it transparent
		///     (ImGui windows with NoBackground / alpha 0) and draw text
		///     or shapes at the screen positions it wants to overlay.
		///   - The helper composites the panel as a full-viewport
		///     alpha-blended quad on each eye buffer, so transparent
		///     pixels show the underlying scene unchanged.
		///   - Same content goes to both eyes (no stereo offset). For
		///     world-anchored elements, the client must project world
		///     positions into screen space and place ImGui content
		///     there itself.
		///   - HUD clients are NOT subject to focus or attachMode
		///     gating; they render every frame they exist. The focused
		///     panel-mode client (if any) renders ON TOP of the HUD
		///     layer as the 3D quad.
		kClientFlag_HUDMode = 1u << 1,
	};

}  // namespace ImGuiVRHelperPluginAPI
