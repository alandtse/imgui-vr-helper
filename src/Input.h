// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Input ingestion + per-frame Frame snapshot construction.
//
// The helper's input pipeline is:
//
//   1. Some upstream feeds VR button/axis events into FeedVREvent().
//      For the SCS-as-first-client phase, SCS pushes events from its own
//      PollInputDevices hook. A future port can install a helper-owned
//      hook to remove that dependency.
//
//   2. FeedVREvent updates Overlay::State::primary/secondaryControllerState
//      so OverlayDrag's grip/depth control logic and combo matching can
//      read live state.
//
//   3. Once per helper tick, BuildFrame() builds an
//      ImGuiVRHelperPluginAPI::Frame from the current state for delivery
//      to each registered client's OnFrame callback. This is where wire
//      bitmasks, edge detection, and pose snapshotting happen.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ImGuiVRHelperTypes.h"

namespace ImGuiVRHelper::Input
{
	/// One recorded VR controller event, for the diagnostics event log
	/// (the helper's "Recent VR controller events" debug table). Captures
	/// only what FeedVREvent receives; `isThumbstick` is inferred from the
	/// key code (axis events aren't button-mapped).
	struct EventLogEntry
	{
		uint32_t device;
		uint32_t keyCode;
		bool pressed;
		bool isThumbstick;
		float thumbstickX;
		float thumbstickY;
	};

	/// Snapshot the recent VR controller event ring buffer, oldest first.
	/// Thread-safe (FeedVREvent writes from the input thread; the settings
	/// UI reads from the render thread).
	std::vector<EventLogEntry> SnapshotEventLog();

	/// Feed one VR input event into the helper's state. `device` is the
	/// game's BSWin32MouseDevice/BSOpenVRControllerDevice device id.
	/// `keyCode` is an `RE::BSOpenVRControllerDevice::Keys::*` value.
	/// For thumbstick events, pass the raw axes; otherwise leave them at 0.
	void FeedVREvent(uint32_t device, uint32_t keyCode, bool pressed,
		float thumbstickX, float thumbstickY);

	/// Force handedness re-detection on next FeedVREvent. Call when the
	/// player changes hand orientation in-game; clears latched state.
	void InvalidateHandedness();

	/// Populate `out` with a wire-shaped Frame snapshot of current state.
	/// Computes pressed/released edges by diffing against the previous
	/// call's bitmasks. `dt` is seconds since previous BuildFrame call.
	void BuildFrame(ImGuiVRHelperPluginAPI::Frame& out, float dt);

	/// Returns true if the player is currently in left-handed mode (last
	/// known value, refreshed by FeedVREvent).
	bool IsLeftHanded();

	/// Reset all latched edge-detection state. Call when transitioning
	/// from hidden to visible overlay so the first visible frame doesn't
	/// look like a flood of edge events.
	void ResetEdgeState();

	/// Human-readable name for an RE::BSOpenVRControllerDevice::Keys code
	/// (Trigger / Grip / A-X / B-Y / ...); "?" for an unmapped code. Shared by
	/// the welcome banner and the settings controller map so they can't drift.
	const char* ButtonName(uint32_t keyCode);

	/// One VR controller button the helper understands. Single source of truth for
	/// the wire-enum mapping, label, canonical fold (Oculus axis-2 alternates
	/// collapse onto their base button), and which surfaces list it — so the combo
	/// recorder, wire mapping, diagnostics table, and ButtonName can't drift apart.
	struct ButtonInfo
	{
		uint32_t reKey;                             // RE::BSOpenVRControllerDevice::Keys value
		ImGuiVRHelperPluginAPI::Button wireButton;  // wire-stable Button bit
		uint32_t canonicalKey;                      // reKey, or the base key an alternate folds onto
		const char* name;                           // human-readable label
		bool candidate;                             // offered for combo recording
		bool diagnostics;                           // listed in the controller diagnostics table
	};

	/// The buttons the helper maps, in canonical order. Iterate this instead of
	/// hand-listing buttons so a new one is added in exactly one place.
	std::span<const ButtonInfo> ButtonTable();

	/// Fold a controller-specific alternate (Oculus grip/touchpad axis-2) onto its
	/// base key so one physical press records once; returns the key unchanged when
	/// it has no alternate.
	uint32_t Canonical(uint32_t keyCode);

	// ---- Synthetic input (devbench bridge) ------------------------------

	/// Queue a synthetic button event, applied to the primary/secondary
	/// controller state on the next input-thread tick — the same state real
	/// events land in, so leases/combos/edge detection see it identically.
	/// Thread-safe (called from devbench's listener thread).
	void InjectButton(bool primaryHand, uint32_t keyCode, bool pressed);

	/// Drain queued synthetic events into controller state. Input thread
	/// only (called from the PollInputDevices thunk).
	void DrainInjected();
}
