// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include "pch.h"

#include "ComboRecording.h"

#include "Overlay.h"

#include <RE/B/BSOpenVRControllerDevice.h>

#include <algorithm>
#include <string>

namespace ImGuiVRHelper::ComboRecording
{
	namespace
	{
		namespace API = ImGuiVRHelperPluginAPI;
		using Keys = RE::BSOpenVRControllerDevice::Keys;

		struct State
		{
			bool active = false;
			uint32_t client_id = 0;
			std::string label;
			API::ComboRecordedFn callback = nullptr;
			void* user = nullptr;
			float timeout_s = 0.0f;
			float elapsed_s = 0.0f;
			std::vector<API::InputCombo> recorded;
			bool any_pressed_this_capture = false;
		};

		State g_state;

		// Same key-set the matcher uses — buttons that map to wire-stable
		// Button enum values via Input.cpp's kButtonMappings.
		constexpr uint32_t kCandidateKeys[] = {
			Keys::kBY,
			Keys::kGrip,
			Keys::kGripAlt,
			Keys::kXA,
			Keys::kJoystickTrigger,
			Keys::kTrigger,
			Keys::kTouchpadClick,
			Keys::kTouchpadAlt,
		};

		bool ContainsCombo(const std::vector<API::InputCombo>& v, const API::InputCombo& c)
		{
			return std::find(v.begin(), v.end(), c) != v.end();
		}

		void Finish(bool deliver)
		{
			if (!g_state.active)
				return;
			auto cb = g_state.callback;
			auto user = g_state.user;
			auto recorded = std::move(g_state.recorded);
			g_state = {};
			if (cb && deliver) {
				cb(recorded.data(), recorded.size(), user);
			} else if (cb) {
				cb(nullptr, 0, user);
			}
		}

		const char* KeyName(uint32_t key)
		{
			switch (key) {
			case Keys::kBY:
				return "B/Y";
			case Keys::kGrip:
				return "Grip";
			case Keys::kGripAlt:
				return "Grip(alt)";
			case Keys::kXA:
				return "X/A";
			case Keys::kJoystickTrigger:
				return "Stick click";
			case Keys::kTrigger:
				return "Trigger";
			case Keys::kTouchpadClick:
				return "Touchpad";
			case Keys::kTouchpadAlt:
				return "Touchpad(alt)";
			default:
				return "?";
			}
		}

		const char* DeviceName(API::InputDeviceType d)
		{
			switch (d) {
			case API::InputDeviceType::Primary:
				return "Pri";
			case API::InputDeviceType::Secondary:
				return "Sec";
			case API::InputDeviceType::Both:
				return "Both";
			default:
				return "?";
			}
		}
	}  // namespace

	void Begin(uint32_t client_id, const char* label, API::ComboRecordedFn on_done,
		void* user, float timeout_s)
	{
		// Cancel any prior session — the modal is single-instance.
		if (g_state.active) {
			Finish(false);
		}
		g_state.active = true;
		g_state.client_id = client_id;
		g_state.label = label ? label : "";
		g_state.callback = on_done;
		g_state.user = user;
		g_state.timeout_s = timeout_s > 0.0f ? timeout_s : 5.0f;
		g_state.elapsed_s = 0.0f;
		g_state.recorded.clear();
		g_state.any_pressed_this_capture = false;
	}

	void Cancel(uint32_t client_id)
	{
		if (!g_state.active)
			return;
		if (g_state.client_id != client_id)
			return;  // not yours
		Finish(false);
	}

	bool IsActive() { return g_state.active; }
	uint32_t ActiveClient() { return g_state.active ? g_state.client_id : 0; }

	void Tick(float dt)
	{
		if (!g_state.active)
			return;

		g_state.elapsed_s += dt;
		if (g_state.elapsed_s >= g_state.timeout_s) {
			// Timeout: deliver whatever we have (could be empty).
			Finish(true);
			return;
		}

		auto& state = Overlay::State::GetSingleton();
		const auto& pri = state.primaryControllerState;
		const auto& sec = state.secondaryControllerState;

		bool anyHeld = false;

		for (const uint32_t key : kCandidateKeys) {
			const bool pHeld = pri[key].isPressed;
			const bool sHeld = sec[key].isPressed;

			anyHeld = anyHeld || pHeld || sHeld;

			// "Both" combo: both controllers held simultaneously — record
			// once and skip per-side recording for this key.
			if (pHeld && sHeld) {
				g_state.any_pressed_this_capture = true;
				const auto bothCombo = API::InputCombo(API::InputDeviceType::Both, key);
				if (!ContainsCombo(g_state.recorded, bothCombo)) {
					// If we already have separate Primary/Secondary entries
					// for this key, replace them with the unified "Both".
					std::erase_if(g_state.recorded, [&](const API::InputCombo& c) {
						return c.GetKey() == key &&
						       (c.GetDevice() == API::InputDeviceType::Primary ||
								   c.GetDevice() == API::InputDeviceType::Secondary);
					});
					g_state.recorded.push_back(bothCombo);
				}
				continue;
			}

			if (pHeld) {
				g_state.any_pressed_this_capture = true;
				const auto c = API::InputCombo(API::InputDeviceType::Primary, key);
				if (!ContainsCombo(g_state.recorded, c)) {
					g_state.recorded.push_back(c);
				}
			}
			if (sHeld) {
				g_state.any_pressed_this_capture = true;
				const auto c = API::InputCombo(API::InputDeviceType::Secondary, key);
				if (!ContainsCombo(g_state.recorded, c)) {
					g_state.recorded.push_back(c);
				}
			}
		}

		// All released after at least one press → finish.
		if (g_state.any_pressed_this_capture && !anyHeld) {
			Finish(true);
		}
	}

	void RenderModal()
	{
		if (!g_state.active)
			return;

		const auto& io = ImGui::GetIO();
		const ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
		ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(600, 350), ImGuiCond_Always);

		ImGui::OpenPopup("##ComboRecordingModal");
		if (ImGui::BeginPopupModal("##ComboRecordingModal", nullptr,
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
					ImGuiWindowFlags_NoMove)) {
			ImGui::TextUnformatted("Press buttons now");
			ImGui::Separator();
			if (!g_state.label.empty()) {
				ImGui::Text("Binding: %s", g_state.label.c_str());
				ImGui::Separator();
			}

			const float remaining = std::max(0.0f, g_state.timeout_s - g_state.elapsed_s);
			ImGui::Text("Time remaining: %.1fs", remaining);
			ImGui::Separator();

			ImGui::TextUnformatted("Recorded so far:");
			if (g_state.recorded.empty()) {
				ImGui::TextDisabled("  (none — press a controller button)");
			} else {
				for (const auto& c : g_state.recorded) {
					ImGui::BulletText("%s %s", DeviceName(c.GetDevice()), KeyName(c.GetKey()));
				}
			}

			ImGui::Spacing();
			ImGui::TextDisabled("Release all buttons to confirm, or wait for timeout.");

			ImGui::Separator();
			if (ImGui::Button("Cancel", ImVec2(120, 36))) {
				Finish(false);
			}
			ImGui::SameLine();
			if (ImGui::Button("Accept", ImVec2(120, 36))) {
				Finish(true);
			}

			ImGui::EndPopup();
		}
	}
}
