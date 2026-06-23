// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include "pch.h"

#include "ComboRecording.h"

#include "Overlay.h"

#include <RE/B/BSOpenVRControllerDevice.h>

#include <algorithm>
#include <cstdio>
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

		// Fold controller-specific alternates onto their canonical key so a single
		// physical button records once. Oculus/Quest report grip as kGripAlt
		// (Axis2) while the matcher also tracks kGrip — without this a single grip
		// captures as "Grip + Grip". Same for the touchpad alternate.
		constexpr uint32_t Canonical(uint32_t key)
		{
			if (key == Keys::kGripAlt)
				return Keys::kGrip;
			if (key == Keys::kTouchpadAlt)
				return Keys::kTouchpadClick;
			return key;
		}

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
				return "Primary";
			case API::InputDeviceType::Secondary:
				return "Secondary";
			case API::InputDeviceType::Both:
				return "Both";
			default:
				return "?";
			}
		}

		// Per-controller color encoding, matching the controller-map UI / client
		// SDK (Primary = yellow, Secondary = blue, Both = green).
		ImVec4 DeviceColor(API::InputDeviceType d)
		{
			switch (d) {
			case API::InputDeviceType::Primary:
				return ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
			case API::InputDeviceType::Secondary:
				return ImVec4(0.0f, 0.5f, 1.0f, 1.0f);
			case API::InputDeviceType::Both:
				return ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
			default:
				return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
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

			// Record under the canonical key so a grip/touchpad alternate folds
			// onto its primary code (no "Grip + Grip" for one physical press).
			const uint32_t ckey = Canonical(key);

			// "Both" combo: both controllers held simultaneously — record
			// once and skip per-side recording for this key.
			if (pHeld && sHeld) {
				g_state.any_pressed_this_capture = true;
				const auto bothCombo = API::InputCombo(API::InputDeviceType::Both, ckey);
				if (!ContainsCombo(g_state.recorded, bothCombo)) {
					// If we already have separate Primary/Secondary entries
					// for this key, replace them with the unified "Both".
					std::erase_if(g_state.recorded, [&](const API::InputCombo& c) {
						return c.GetKey() == ckey &&
						       (c.GetDevice() == API::InputDeviceType::Primary ||
								   c.GetDevice() == API::InputDeviceType::Secondary);
					});
					g_state.recorded.push_back(bothCombo);
				}
				continue;
			}

			if (pHeld) {
				g_state.any_pressed_this_capture = true;
				const auto c = API::InputCombo(API::InputDeviceType::Primary, ckey);
				if (!ContainsCombo(g_state.recorded, c)) {
					g_state.recorded.push_back(c);
				}
			}
			if (sHeld) {
				g_state.any_pressed_this_capture = true;
				const auto c = API::InputCombo(API::InputDeviceType::Secondary, ckey);
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
		ImGui::SetNextWindowSize(ImVec2(640, 0), ImGuiCond_Always);

		// Distinct rebind dialog: gold accent border + heading so it can't be
		// mistaken for the normal settings menu when it takes over the panel.
		const ImVec4 accent(1.0f, 0.78f, 0.20f, 1.0f);
		ImGui::PushStyleColor(ImGuiCol_Border, accent);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 3.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 20));

		ImGui::OpenPopup("##ComboRecordingModal");
		if (ImGui::BeginPopupModal("##ComboRecordingModal", nullptr,
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
					ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::PushStyleColor(ImGuiCol_Text, accent);
			ImGui::SetWindowFontScale(1.6f);
			ImGui::TextUnformatted("REBIND");
			ImGui::SetWindowFontScale(1.0f);
			ImGui::PopStyleColor();
			if (!g_state.label.empty()) {
				ImGui::SameLine(0.0f, 10.0f);
				ImGui::AlignTextToFramePadding();
				ImGui::Text("%s", g_state.label.c_str());
			}
			ImGui::Separator();
			ImGui::Spacing();

			ImGui::TextWrapped("Hold the new button combo on your controller, then release to confirm.");
			ImGui::Spacing();

			// Countdown bar.
			const float remaining = std::max(0.0f, g_state.timeout_s - g_state.elapsed_s);
			const float frac = g_state.timeout_s > 0.0f ? remaining / g_state.timeout_s : 0.0f;
			char buf[32];
			std::snprintf(buf, sizeof(buf), "%.1fs", remaining);
			ImGui::PushStyleColor(ImGuiCol_PlotHistogram, accent);
			ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f), buf);
			ImGui::PopStyleColor();
			ImGui::Spacing();

			// Captured combo: larger + color-coded per controller.
			ImGui::TextDisabled("New combo:");
			ImGui::SetWindowFontScale(1.3f);
			if (g_state.recorded.empty()) {
				ImGui::TextDisabled("  (press a controller button)");
			} else {
				ImGui::TextUnformatted("  ");
				for (std::size_t i = 0; i < g_state.recorded.size(); ++i) {
					const auto& c = g_state.recorded[i];
					ImGui::SameLine(0.0f, i == 0 ? 0.0f : 6.0f);
					if (i != 0) {
						ImGui::TextDisabled("+");
						ImGui::SameLine(0.0f, 6.0f);
					}
					ImGui::TextColored(DeviceColor(c.GetDevice()), "%s %s",
						DeviceName(c.GetDevice()), KeyName(c.GetKey()));
				}
			}
			ImGui::SetWindowFontScale(1.0f);

			ImGui::Spacing();
			ImGui::Separator();
			if (ImGui::Button("Cancel", ImVec2(140, 40))) {
				Finish(false);
			}
			ImGui::SameLine();
			if (ImGui::Button("Accept", ImVec2(140, 40))) {
				Finish(true);
			}

			ImGui::EndPopup();
		}

		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor(1);
	}
}
