// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include "pch.h"

#include "SettingsUI.h"

#include "ComboRecording.h"
#include "Globals.h"
#include "HelperImpl.h"
#include "Overlay.h"
#include "WandPointing.h"

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
				// Label deliberately differs from the enclosing
				// CollapsingHeader("Positioning") — ImGui hashes the
				// label into a widget ID, and two widgets in the same
				// window with the same label collide.
				if (ImGui::Combo("Positioning method", &methodIndex, methodLabels, IM_ARRAYSIZE(methodLabels))) {
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

				ImGui::Separator();
				ImGui::TextUnformatted("Toggle hotkey: Shift+F2 (keyboard).");
				ImGui::TextDisabled("Click below to bind a controller combo too.");
				if (ImGui::Button("Rebind toggle key")) {
					auto& impl = HelperImpl::GetSingleton();
					ComboRecording::Begin(impl.GetSelfClientId(), "ImGuiVRHelper toggle", +[](const ImGuiVRHelperPluginAPI::InputCombo* keys, std::size_t n, void* /*user*/) {
							if (n == 0) return;  // user cancelled or timed out empty
							HelperImpl::GetSingleton().RebindSelfToggle(keys, n); }, nullptr, 5.0f);
				}
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
		// Just flip the visibility flag. Focus management and settings
		// persistence are owned by HelperImpl::DispatchFrame's
		// SyncSelfFocus reconciler, which fires every frame and catches
		// every close path (this Toggle, the X button on the ImGui
		// window, programmatic visibility flips). Keeping Toggle a pure
		// flag-flip keeps the close paths from each having their own
		// version of the cleanup logic.
		g_visible = !g_visible;
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

		// Two cursor sources, matching SCS open_composite exactly:
		//
		// 1. Wand pointing: UpdateCursorFromWandPointing raycasts the
		//    OPPOSITE controller's forward vector against the overlay
		//    panel's internal transform model and feeds the UV*size
		//    pixel coords as a mouse position. Mirrors SCS's
		//    VR::UpdateCursorFromWandPointing
		//    (origin/open_composite src/Features/VR/WandPointing.cpp:104-145).
		//
		// 2. Thumbstick deflection driving cursor delta — also injects
		//    AddMousePosEvent. Last-writer-wins per frame, so an active
		//    stick deflection overrides a stale wand hit, and a steady
		//    wand hit overrides a centered stick. Matches SCS Input.cpp.
		//
		// Buttons: trigger=left, grip=right, joystick-click/touchpad=middle.
		// kBY=Tab, kXA=Enter — keyboard shortcuts for menu navigation.
		// Edge-detected per controller so we only fire transitions.
		auto& vrState = Overlay::State::GetSingleton();
		const auto& settings = vrState.settings;

		WandPointing::UpdateCursorFromWandPointing();

		auto driveCursorAndScrollFrom = [&](const RE::VRControllerState& cursorCtl,
											const RE::VRControllerState& scrollCtl,
											size_t cursorThumbIdx, size_t scrollThumbIdx) {
			// Cursor (SCS uses ~10 px/frame at full deflection).
			constexpr float kMouseSpeed = 12.0f;
			const float cx = cursorCtl.thumbsticks[cursorThumbIdx].x;
			const float cy = cursorCtl.thumbsticks[cursorThumbIdx].y;
			if (std::abs(cx) > settings.mouseDeadzone || std::abs(cy) > settings.mouseDeadzone) {
				ImVec2 pos = io.MousePos;
				if (pos.x < 0.0f || pos.x > io.DisplaySize.x ||
					pos.y < 0.0f || pos.y > io.DisplaySize.y) {
					// Cursor out of range / never set — center it so the
					// first stick deflection doesn't teleport from -FLT_MAX.
					pos.x = io.DisplaySize.x * 0.5f;
					pos.y = io.DisplaySize.y * 0.5f;
				}
				pos.x += cx * kMouseSpeed;
				pos.y -= cy * kMouseSpeed;  // stick up = cursor up
				pos.x = std::clamp(pos.x, 0.0f, io.DisplaySize.x);
				pos.y = std::clamp(pos.y, 0.0f, io.DisplaySize.y);
				io.AddMousePosEvent(pos.x, pos.y);
				io.MouseDrawCursor = true;
			}

			// Scroll: discrete tick when the other stick crosses 0.3
			// accumulated deflection (SCS pattern, prevents 90Hz spam).
			static float scrollAccumX = 0.0f;
			static float scrollAccumY = 0.0f;
			const float sx = scrollCtl.thumbsticks[scrollThumbIdx].x;
			const float sy = scrollCtl.thumbsticks[scrollThumbIdx].y;
			if (std::abs(sx) > settings.mouseDeadzone)
				scrollAccumX += sx * 0.1f;
			if (std::abs(sy) > settings.mouseDeadzone)
				scrollAccumY += sy * 0.1f;
			float wheelX = 0.0f, wheelY = 0.0f;
			if (std::abs(scrollAccumX) > 0.3f) {
				wheelX = scrollAccumX > 0 ? 1.0f : -1.0f;
				scrollAccumX = 0.0f;
			}
			if (std::abs(scrollAccumY) > 0.3f) {
				wheelY = scrollAccumY > 0 ? 1.0f : -1.0f;
				scrollAccumY = 0.0f;
			}
			if (wheelX != 0.0f || wheelY != 0.0f) {
				io.AddMouseWheelEvent(wheelX, wheelY);
			}
		};

		// Pick which controller drives cursor vs scroll. When the menu is
		// attached to a controller, that hand's stick is awkward to reach
		// while holding the menu, so the OPPOSITE hand drives the cursor
		// (SCS does this swap explicitly). Otherwise primary=cursor,
		// secondary=scroll.
		namespace API = ImGuiVRHelperPluginAPI;
		const bool menuOnPrimary = (settings.attachMode == Overlay::AttachMode::ControllerOnly &&
									settings.attachController == API::InputDeviceType::Primary);
		if (menuOnPrimary) {
			driveCursorAndScrollFrom(vrState.secondaryControllerState,
				vrState.primaryControllerState, 1, 0);  // secondary stick=cursor, primary stick=scroll
		} else {
			driveCursorAndScrollFrom(vrState.primaryControllerState,
				vrState.secondaryControllerState, 0, 1);
		}

		// Button → mouse/key edge-detector. Tracks per-controller per-key
		// previous state so simultaneous presses on both hands don't
		// double-fire.
		struct ButtonMap
		{
			uint32_t reKey;
			int imguiButton;  // -1 if this is a key event
			ImGuiKey key;     // valid only when imguiButton == -1
			bool shift;       // hold Shift while sending key
		};
		using K = RE::BSOpenVRControllerDevice::Keys;
		static const ButtonMap kMappings[] = {
			{ K::kTrigger, ImGuiMouseButton_Left, ImGuiKey_None, false },
			// kGrip is the toggle combo when both controllers grip
			// simultaneously, but a single-hand grip should still work as
			// right-click. The toggle combo's matcher only fires on
			// simultaneous-rising-edge so the overlap is benign.
			{ K::kGrip, ImGuiMouseButton_Right, ImGuiKey_None, false },
			{ K::kGripAlt, ImGuiMouseButton_Right, ImGuiKey_None, false },
			{ K::kTouchpadClick, ImGuiMouseButton_Middle, ImGuiKey_None, false },
			{ K::kJoystickTrigger, ImGuiMouseButton_Middle, ImGuiKey_None, false },
			{ K::kBY, -1, ImGuiKey_Tab, true },
			{ K::kXA, -1, ImGuiKey_Enter, false },
		};
		auto pumpButtons = [&](const RE::VRControllerState& ctlState, bool* prev) {
			for (size_t i = 0; i < std::size(kMappings); ++i) {
				const auto& m = kMappings[i];
				const bool pressed = ctlState[m.reKey].isPressed;
				if (pressed != prev[i]) {
					if (m.imguiButton >= 0) {
						io.AddMouseButtonEvent(m.imguiButton, pressed);
					} else {
						if (m.shift)
							io.AddKeyEvent(ImGuiMod_Shift, pressed);
						io.AddKeyEvent(m.key, pressed);
					}
					prev[i] = pressed;
				}
			}
		};
		static bool prevPrimary[std::size(kMappings)] = {};
		static bool prevSecondary[std::size(kMappings)] = {};
		pumpButtons(vrState.primaryControllerState, prevPrimary);
		pumpButtons(vrState.secondaryControllerState, prevSecondary);

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
