// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include "pch.h"

#include "ComboRecording.h"

#include "Globals.h"
#include "Overlay.h"
#include "Theme.h"

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

		// Dedicated, non-interactive ImGui context (own font atlas + DX11 backend,
		// per-context in imgui 1.92) so the capture overlay never touches the menu
		// contexts. Mirrors ToastHUD — the helper composites this panel on top of
		// the focused client in a dedicated pass.
		ImGuiContext* g_renderCtx = nullptr;
		bool g_dx11Inited = false;

		constexpr float kPanelWidth = static_cast<float>(Overlay::Config::kOverlayWidth);
		constexpr float kPanelHeight = static_cast<float>(Overlay::Config::kOverlayHeight);

		bool EnsureRenderInitialized()
		{
			if (g_renderCtx && g_dx11Inited)
				return true;
			if (!Globals::IsReady())
				return false;

			if (!g_renderCtx) {
				IMGUI_CHECKVERSION();
				g_renderCtx = ImGui::CreateContext();
				ImGui::SetCurrentContext(g_renderCtx);
				ImGuiIO& io = ImGui::GetIO();
				io.IniFilename = nullptr;
				io.LogFilename = nullptr;
				io.DisplaySize = ImVec2(kPanelWidth, kPanelHeight);
				Theme::Apply(ImGui::GetStyle());
				ImGui::GetStyle().ScaleAllSizes(2.0f);
				io.FontGlobalScale = 1.5f;
			}

			if (!g_dx11Inited) {
				ImGui::SetCurrentContext(g_renderCtx);
				auto& d3d = Globals::GetD3D();
				if (!ImGui_ImplDX11_Init(d3d.device, d3d.context)) {
					logs::error("ComboRecording: ImGui_ImplDX11_Init failed");
					return false;
				}
				g_dx11Inited = true;
			}
			return true;
		}

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

	// Emit the dialog contents into the current frame of the dedicated context.
	static void DrawDialog()
	{
		ImGuiIO& io = ImGui::GetIO();

		// Dim the whole panel behind the dialog (composited over the focused
		// client, so this reads as a standard modal scrim).
		ImGui::GetBackgroundDrawList()->AddRectFilled(
			ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 150));

		const ImVec4 accent(1.0f, 0.78f, 0.20f, 1.0f);  // gold
		ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
			ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(640, 0), ImGuiCond_Always);
		ImGui::PushStyleColor(ImGuiCol_Border, accent);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 3.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 20));

		constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
		                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
		                                   ImGuiWindowFlags_NoInputs;
		if (ImGui::Begin("##RebindCapture", nullptr, flags)) {
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

			ImGui::TextWrapped("Hold the new button combo, then release to confirm. Press nothing to keep the current binding.");
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
		}
		ImGui::End();

		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor(1);
	}

	void RenderToPanel(ID3D11RenderTargetView* rtv)
	{
		if (!g_state.active || !rtv)
			return;
		if (!EnsureRenderInitialized())
			return;

		ImGuiContext* prev = ImGui::GetCurrentContext();
		ImGui::SetCurrentContext(g_renderCtx);

		ImGui::GetIO().DeltaTime = 1.0f / 60.0f;  // animation-free; dt unused
		ImGui_ImplDX11_NewFrame();
		ImGui::NewFrame();
		DrawDialog();
		ImGui::Render();

		auto* ctx = Globals::GetD3D().context;
		ID3D11RenderTargetView* oldRTV = nullptr;
		ID3D11DepthStencilView* oldDSV = nullptr;
		ctx->OMGetRenderTargets(1, &oldRTV, &oldDSV);
		const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		ctx->OMSetRenderTargets(1, &rtv, nullptr);
		ctx->ClearRenderTargetView(rtv, clear);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		ctx->OMSetRenderTargets(1, &oldRTV, oldDSV);
		if (oldRTV)
			oldRTV->Release();
		if (oldDSV)
			oldDSV->Release();

		if (prev != g_renderCtx)
			ImGui::SetCurrentContext(prev);
	}

	void ClearToTransparent(ID3D11RenderTargetView* rtv)
	{
		if (!rtv || !Globals::IsReady())
			return;
		const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		Globals::GetD3D().context->ClearRenderTargetView(rtv, clear);
	}
}
