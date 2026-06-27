// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include "pch.h"

#include "VRKeyboard.h"

#include <RE/B/BSOpenVR.h>
#include <openvr.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>

namespace ImGuiVRHelper::VRKeyboard
{
	namespace
	{
		std::mutex g_mutex;

		// OpenComposite floors its keyboard buffer at one character: deleting the last
		// character produces no event and GetKeyboardText keeps returning it, so the
		// field can never be cleared. We seed an invisible U+00A0 (NBSP) floor char in
		// front of the real text; the runtime's "can't delete the last char" then
		// bottoms out on the sentinel, and we strip it before reporting text, so the
		// client/field never see it but the user can delete every real character.
		const std::string kSentinel = "\xC2\xA0";  // U+00A0 in UTF-8

		// Desired state, written by the render thread (SetActive).
		bool g_wantActive = false;
		uint32_t g_wantClient = 0;
		std::string g_seed;
		bool g_seedPending = false;

		// Shared text + close edge, written by the input thread, read by render.
		std::string g_currentText;
		bool g_closed = false;

		// Input-thread-only.
		bool g_shown = false;
		uint32_t g_shownClient = 0;
		// Set once a real keystroke has arrived. After that we poll the live text
		// every tick (not just on char events) as a safety net for runtimes that may
		// mutate the buffer without firing an event. Before the first keystroke we
		// keep the seed echo so polling can't wipe pre-existing field text.
		bool g_live = false;
		// Spurious-close guard: OpenComposite fires a bare Closed ~15 ms after the
		// very first ShowKeyboardForOverlay (cold anchor), which would otherwise reach
		// the client as a dismiss. If a close arrives within this window before any
		// keystroke, re-show transparently instead. Capped so a runtime that truly
		// refuses to keep the keyboard open can't spin forever.
		constexpr int kShowGuardTicks = 30;
		constexpr int kMaxReshows = 3;
		int g_showGuard = 0;
		int g_reshowsLeft = 0;
		bool g_suppressReopen = false;  // set on Done so the still-focused field can't re-pop
		// Opening the keyboard steals focus from the ImGui field, so WantTextInput
		// (and thus the client's active flag) flickers off for a frame or two. Latch
		// the keyboard shown for a grace window after the last "active" tick so it
		// doesn't flicker open/closed; only hide once truly unfocused.
		constexpr int kHideGraceTicks = 120;
		int g_hideGrace = 0;
		vr::VROverlayHandle_t g_anchor = vr::k_ulOverlayHandleInvalid;
		bool g_failLogged = false;

		vr::IVROverlay* GetOverlay()
		{
			auto* openvr = RE::BSOpenVR::GetSingleton();
			if (!openvr || !openvr->vrSystem)
				return nullptr;
			return RE::BSOpenVR::GetIVROverlayFromContext(&openvr->vrContext);
		}

		bool EnsureAnchor(vr::IVROverlay* ov)
		{
			if (g_anchor != vr::k_ulOverlayHandleInvalid)
				return true;
			if (ov->CreateOverlay("imguivrhelper.kbd.anchor", "ImGuiVRHelper Keyboard", &g_anchor) !=
				vr::VROverlayError_None) {
				g_anchor = vr::k_ulOverlayHandleInvalid;
				return false;
			}
			// Anchor ~0.6 m in front of and slightly below the HMD; the keyboard is
			// positioned relative to it. Tuned in-headset.
			vr::HmdMatrix34_t m{};
			m.m[0][0] = 1.0f;
			m.m[1][1] = 1.0f;
			m.m[2][2] = 1.0f;
			m.m[1][3] = -0.3f;  // down
			m.m[2][3] = -0.6f;  // forward (-z)
			ov->SetOverlayTransformTrackedDeviceRelative(g_anchor, vr::k_unTrackedDeviceIndex_Hmd, &m);
			return true;
		}

		void ReadKeyboardText(vr::IVROverlay* ov)
		{
			char buf[1024];
			const uint32_t len = ov->GetKeyboardText(buf, sizeof(buf));
			const uint32_t n = std::min<uint32_t>(len, sizeof(buf) - 1);
			std::string t(buf, n);
			// Strip the leading sentinel floor char. If it is gone (some runtimes do
			// let the buffer empty), treat whatever remains as the real text.
			if (t.rfind(kSentinel, 0) == 0)
				t.erase(0, kSentinel.size());
			std::scoped_lock lk(g_mutex);
			g_currentText = std::move(t);
		}
	}

	void SetActive(uint32_t clientId, bool active, const char* seed)
	{
		std::scoped_lock lk(g_mutex);
		if (active) {
			const bool rising = !g_wantActive || g_wantClient != clientId;
			g_wantActive = true;
			g_wantClient = clientId;
			if (rising) {
				g_seed = seed ? seed : "";
				g_seedPending = true;
				g_currentText = g_seed;  // echo the seed until the runtime updates it
			}
		} else if (g_wantClient == clientId) {
			g_wantActive = false;
			g_wantClient = 0;
		}
	}

	uint32_t GetText(uint32_t clientId, char* out, uint32_t outSize)
	{
		if (!out || outSize == 0)
			return 0;
		std::scoped_lock lk(g_mutex);
		if (clientId != g_wantClient && clientId != g_shownClient) {
			out[0] = 0;
			return 0;
		}
		const uint32_t n = std::min<uint32_t>(outSize - 1, static_cast<uint32_t>(g_currentText.size()));
		std::memcpy(out, g_currentText.data(), n);
		out[n] = 0;
		return n;
	}

	bool ConsumeClosed(uint32_t clientId)
	{
		std::scoped_lock lk(g_mutex);
		if (g_closed && (clientId == g_wantClient || clientId == g_shownClient)) {
			g_closed = false;
			return true;
		}
		return false;
	}

	void Tick()
	{
		bool wantActive;
		uint32_t wantClient;
		std::string seed;
		bool seedPending;
		{
			std::scoped_lock lk(g_mutex);
			wantActive = g_wantActive;
			wantClient = g_wantClient;
			seed = g_seed;
			seedPending = g_seedPending;
			g_seedPending = false;
		}

		vr::IVROverlay* ov = GetOverlay();
		if (!ov)
			return;

		// A fresh activating edge clears the post-Done suppression.
		if (seedPending)
			g_suppressReopen = false;

		if (wantActive)
			g_hideGrace = kHideGraceTicks;  // keep shown while focused (+ grace)

		if (wantActive && !g_shown && !g_suppressReopen) {
			if (!EnsureAnchor(ov)) {
				if (!g_failLogged) {
					logs::warn("VRKeyboard: CreateOverlay failed; VR keyboard unavailable");
					g_failLogged = true;
				}
				return;
			}
			const std::string shownSeed = kSentinel + seed;  // floor char + real text
			const vr::EVROverlayError err = ov->ShowKeyboardForOverlay(g_anchor,
				vr::k_EGamepadTextInputModeNormal, vr::k_EGamepadTextInputLineModeSingleLine,
				"ImGuiVRHelper", 256, shownSeed.c_str(), false, 0);
			if (err != vr::VROverlayError_None) {
				if (!g_failLogged) {
					logs::warn("VRKeyboard: ShowKeyboardForOverlay failed ({}); runtime has no keyboard",
						static_cast<int>(err));
					g_failLogged = true;
				}
				return;
			}
			g_shown = true;
			g_shownClient = wantClient;
			g_live = false;  // trust polling only after the first keystroke
			g_showGuard = kShowGuardTicks;
			g_reshowsLeft = kMaxReshows;
		}

		if (g_shown && g_anchor != vr::k_ulOverlayHandleInvalid) {
			vr::VREvent_t ev{};
			while (ov->PollNextOverlayEvent(g_anchor, &ev, sizeof(ev))) {
				switch (ev.eventType) {
				case vr::VREvent_KeyboardCharInput:
					ReadKeyboardText(ov);
					g_live = true;
					break;
				case vr::VREvent_KeyboardClosed:
				case vr::VREvent_KeyboardDone:
					// A bare Closed within the guard window, before any keystroke, is the
					// cold-show glitch: re-show transparently instead of reporting it.
					if (ev.eventType == vr::VREvent_KeyboardClosed && g_showGuard > 0 && !g_live &&
						g_reshowsLeft > 0) {
						--g_reshowsLeft;
						std::string cur;
						{
							std::scoped_lock lk(g_mutex);
							cur = g_currentText;
						}
						const std::string shownSeed = kSentinel + cur;
						ov->ShowKeyboardForOverlay(g_anchor, vr::k_EGamepadTextInputModeNormal,
							vr::k_EGamepadTextInputLineModeSingleLine, "ImGuiVRHelper", 256,
							shownSeed.c_str(), false, 0);
						g_showGuard = kShowGuardTicks;
						break;
					}
					ReadKeyboardText(ov);
					ov->HideKeyboard();
					g_shown = false;
					g_hideGrace = 0;
					g_suppressReopen = true;
					{
						std::scoped_lock lk(g_mutex);
						g_closed = true;
					}
					break;
				default:
					break;
				}
			}
			// Safety net: poll the live text every tick once typing has started, in
			// case a runtime mutates the buffer without firing a char event for it.
			if (g_shown && g_live)
				ReadKeyboardText(ov);
		}

		if (g_showGuard > 0)
			--g_showGuard;

		if (!wantActive && g_hideGrace > 0)
			--g_hideGrace;

		if (!wantActive && g_hideGrace == 0) {
			g_suppressReopen = false;
			if (g_shown) {
				ov->HideKeyboard();
				g_shown = false;
			}
		}
	}

	void Shutdown()
	{
		if (vr::IVROverlay* ov = GetOverlay()) {
			if (g_shown)
				ov->HideKeyboard();
			if (g_anchor != vr::k_ulOverlayHandleInvalid)
				ov->DestroyOverlay(g_anchor);
		}
		g_shown = false;
		g_anchor = vr::k_ulOverlayHandleInvalid;
	}
}
