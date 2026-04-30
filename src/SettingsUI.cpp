// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include "pch.h"

#include "SettingsUI.h"

#include "ComboRecording.h"
#include "Globals.h"
#include "Overlay.h"

namespace ImGuiVRHelper::SettingsUI
{
	namespace
	{
		ImGuiContext* g_ctx = nullptr;
		bool g_visible = false;

		void RenderWindow()
		{
			auto& state = Overlay::State::GetSingleton();
			auto& s = state.settings;

			ImGui::SetNextWindowSize(ImVec2(700, 600), ImGuiCond_FirstUseEver);
			if (!ImGui::Begin("ImGuiVRHelper Settings", &g_visible)) {
				ImGui::End();
				return;
			}

			ImGui::TextUnformatted("Drag this window with controller grip to reposition the overlay.");
			ImGui::Separator();

			if (ImGui::CollapsingHeader("Positioning", ImGuiTreeNodeFlags_DefaultOpen)) {
				const char* attachLabels[] = { "HMD only", "Controller only", "Both", "None" };
				int attachIndex = static_cast<int>(s.attachMode);
				if (ImGui::Combo("Attach mode", &attachIndex, attachLabels, IM_ARRAYSIZE(attachLabels))) {
					s.attachMode = static_cast<Overlay::AttachMode>(attachIndex);
				}

				const char* methodLabels[] = { "HMD-relative", "Fixed in world" };
				int methodIndex = static_cast<int>(s.positioningMethod);
				if (ImGui::Combo("Positioning", &methodIndex, methodLabels, IM_ARRAYSIZE(methodLabels))) {
					s.positioningMethod = static_cast<Overlay::PositioningMethod>(methodIndex);
				}

				ImGui::SliderFloat("Scale (m wide)", &s.menuScale,
					Overlay::Config::kMinMenuScale, Overlay::Config::kMaxMenuScale, "%.2f");
			}

			if (ImGui::CollapsingHeader("HMD-relative offsets")) {
				ImGui::SliderFloat("X##hmd", &s.hmdOffsetX, -2.0f, 2.0f, "%.3f m");
				ImGui::SliderFloat("Y##hmd", &s.hmdOffsetY, -2.0f, 2.0f, "%.3f m");
				ImGui::SliderFloat("Z##hmd", &s.hmdOffsetZ, -3.0f, 0.0f, "%.3f m");
			}

			if (ImGui::CollapsingHeader("Controller-relative offsets")) {
				ImGui::SliderFloat("X##ctrl", &s.controllerOffsetX, -1.0f, 1.0f, "%.3f m");
				ImGui::SliderFloat("Y##ctrl", &s.controllerOffsetY, -1.0f, 1.0f, "%.3f m");
				ImGui::SliderFloat("Z##ctrl", &s.controllerOffsetZ, -1.0f, 1.0f, "%.3f m");

				const char* sideLabels[] = { "Primary", "Secondary" };
				int sideIndex = (s.attachController == ImGuiVRHelperPluginAPI::InputDeviceType::Primary) ? 0 : 1;
				if (ImGui::Combo("Attach to", &sideIndex, sideLabels, IM_ARRAYSIZE(sideLabels))) {
					s.attachController = sideIndex == 0 ?
					                         ImGuiVRHelperPluginAPI::InputDeviceType::Primary :
					                         ImGuiVRHelperPluginAPI::InputDeviceType::Secondary;
				}
			}

			if (ImGui::CollapsingHeader("Interaction")) {
				ImGui::Checkbox("Wand pointer (laser cursor)", &s.enableWandPointing);
				ImGui::Checkbox("Grip-to-drag repositioning", &s.enableDragToReposition);
				ImGui::SliderFloat("Mouse deadzone", &s.mouseDeadzone, 0.0f, 1.0f, "%.2f");
				ImGui::SliderFloat("Auto-reset distance",
					&s.autoResetDistance, 0.0f, 5000.0f, "%.0f units");
			}

			if (ImGui::CollapsingHeader("Diagnostics")) {
				ImGui::Text("Overlay visible: %s", state.overlayVisible ? "yes" : "no");
				ImGui::Text("Left-handed: %s", state.lastKnownLeftHandedMode ? "yes" : "no");
				ImGui::Text("Wand intersecting: %s",
					state.wandState.isIntersecting ? "yes" : "no");
				if (state.wandState.isIntersecting) {
					ImGui::Text("  UV: (%.3f, %.3f)",
						state.wandState.uvCoordinates.x, state.wandState.uvCoordinates.y);
				}
				ImGui::Text("Drag active: %s", state.dragState.dragging ? "yes" : "no");
			}

			ImGui::End();
		}
	}  // namespace

	bool Init()
	{
		if (g_ctx)
			return true;
		if (!Globals::IsReady())
			return false;

		auto& d3d = Globals::GetD3D();
		IMGUI_CHECKVERSION();
		g_ctx = ImGui::CreateContext();
		ImGui::SetCurrentContext(g_ctx);

		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;  // disable imgui.ini auto-save
		io.LogFilename = nullptr;
		io.DisplaySize = ImVec2(static_cast<float>(Overlay::Config::kOverlayWidth),
			static_cast<float>(Overlay::Config::kOverlayHeight));
		io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

		// Mouse-cursor scaling so a 1920x1080 panel looks reasonable in VR.
		ImGui::GetStyle().ScaleAllSizes(2.0f);
		io.FontGlobalScale = 1.5f;

		if (!ImGui_ImplDX11_Init(d3d.device, d3d.context)) {
			logs::error("SettingsUI::Init: ImGui_ImplDX11_Init failed");
			ImGui::DestroyContext(g_ctx);
			g_ctx = nullptr;
			return false;
		}

		logs::info("SettingsUI initialized (helper ImGui context @ {})", static_cast<void*>(g_ctx));
		return true;
	}

	void Shutdown()
	{
		if (!g_ctx)
			return;
		ImGui::SetCurrentContext(g_ctx);
		ImGui_ImplDX11_Shutdown();
		ImGui::DestroyContext(g_ctx);
		g_ctx = nullptr;
		g_visible = false;
	}

	bool IsInitialized() { return g_ctx != nullptr; }

	void Toggle()
	{
		const bool wasVisible = g_visible;
		g_visible = !g_visible;
		// Persist on every close — it's cheap (small JSON) and avoids
		// silent loss of changes if Skyrim crashes before a graceful exit.
		if (wasVisible && !g_visible) {
			Overlay::SaveSettings();
		}
	}

	bool IsVisible() { return g_visible; }

	bool Render(float dt)
	{
		if (!g_ctx)
			return false;
		// Render whenever either the settings window or the combo recording
		// modal is up. The modal can fire from any client at any time.
		if (!g_visible && !ComboRecording::IsActive())
			return false;

		ImGui::SetCurrentContext(g_ctx);

		ImGuiIO& io = ImGui::GetIO();
		io.DeltaTime = dt > 0.0f ? dt : 1.0f / 60.0f;

		// Drive mouse position from the wand pointer when it's intersecting
		// the panel. Trigger acts as left-click; this is the helper's own
		// internal input plumbing — it doesn't go through the public
		// GetPointer / FeedVREvent paths because the helper isn't a client
		// of itself in that sense, just a renderer that lives in the same
		// process.
		auto& state = Overlay::State::GetSingleton();
		if (state.wandState.isIntersecting) {
			const float x = state.wandState.uvCoordinates.x * io.DisplaySize.x;
			const float y = state.wandState.uvCoordinates.y * io.DisplaySize.y;
			io.AddMousePosEvent(x, y);
		} else {
			io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
		}

		// Trigger → left mouse button (edge-detected from controller state).
		static bool lastTriggerHeld = false;
		const bool triggerHeld =
			state.primaryControllerState[RE::BSOpenVRControllerDevice::Keys::kTrigger].isPressed ||
			state.secondaryControllerState[RE::BSOpenVRControllerDevice::Keys::kTrigger].isPressed;
		if (triggerHeld != lastTriggerHeld) {
			io.AddMouseButtonEvent(0, triggerHeld);
			lastTriggerHeld = triggerHeld;
		}

		ImGui_ImplDX11_NewFrame();
		ImGui::NewFrame();
		if (g_visible) {
			RenderWindow();
		}
		ComboRecording::RenderModal();
		ImGui::Render();
		return true;
	}
}
