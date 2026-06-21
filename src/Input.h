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
}
