// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// VR text-entry bridge. Backs the IImGuiVRHelperInterface002 keyboard methods.
//
// Threading: all IVROverlay work (ShowKeyboardForOverlay, PollNextOverlayEvent,
// GetKeyboardText, HideKeyboard) runs on the input thread via Tick(), called from
// the PollInputDevices hook — calling IVROverlay from the render thread is the
// "device or resource busy" race. The render-thread setters/getters only touch a
// mutex-guarded buffer + atomics. Keyboard events are drained from a helper-owned
// anchor overlay's queue (PollNextOverlayEvent), not the system queue, so the
// game's own VR events are never stolen.

#pragma once

#include <cstdint>

namespace ImGuiVRHelper::VRKeyboard
{
	/// Request (active=true) or dismiss (active=false) the VR keyboard for the
	/// given client's focused text field. Call every active frame; `seed` is the
	/// field's current text, applied on the activating edge. Render-thread safe.
	void SetActive(uint32_t clientId, bool active, const char* seed);

	/// Copy the keyboard's current text (UTF-8, null-terminated) into `out` and
	/// return its byte length. The runtime keyboard is authoritative. 0 if no text
	/// or this client isn't the active one. Render-thread safe.
	uint32_t GetText(uint32_t clientId, char* out, uint32_t outSize);

	/// Returns (and clears) true once after the user closed the keyboard with
	/// Done/X for this client, so the SDK can commit + defocus its field.
	bool ConsumeClosed(uint32_t clientId);

	/// Input-thread pump (from PollInputDevices). Owns every IVROverlay call.
	void Tick();

	/// Hide the keyboard and destroy the anchor overlay. Safe anytime.
	void Shutdown();
}
