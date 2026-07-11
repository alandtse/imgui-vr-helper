// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include "pch.h"

#include "SettingsUI.h"

#include "ComboRecording.h"
#include "Globals.h"
#include "HelperImpl.h"
#include "HudContext.h"
#include "HudDisplay.h"
#include "Input.h"
#include "OpenVRDetection.h"
#include "Overlay.h"
#include "PluginVersion.h"
#include "Theme.h"
#include "WandPointing.h"

#include <RE/B/BSOpenVR.h>
#include <algorithm>
#include <chrono>
#include <dxgi.h>
#include <imgui_impl_win32.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
	HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace ImGuiVRHelper::SettingsUI
{
	namespace
	{
		ImGuiContext* g_ctx = nullptr;
		bool g_visible = false;

		// Win32 input plumbing. We hook the swapchain window's WndProc
		// so desktop mouse + keyboard reach the helper's ImGui context.
		// Lets users drag sliders with a mouse, type values with a
		// keyboard, copy/paste — same interaction story flat-Skyrim
		// users get for any ImGui-based mod.
		HWND g_hwnd = nullptr;
		WNDPROC g_origWndProc = nullptr;

		LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
		{
			// Forward every Win32 message to ImGui's input backend
			// against WHATEVER context is currently active — DO NOT
			// mutate GImGui from this thread.
			//
			// Earlier versions called SetCurrentContext(g_ctx) here so
			// settings_ctx would receive Win32 events even if the
			// render thread had switched to a different context (e.g.
			// HUDDemo's). That introduced a multi-thread race: Skyrim
			// dispatches WndProc from a different thread than Present
			// (rendering happens on a JobListManager::ServingThread,
			// not the message pump's thread), so swapping GImGui from
			// the WndProc thread mid-render-frame would null-deref the
			// render thread's NewFrame call when its read of GImGui
			// landed during our swap window. Crash repro:
			//   imgui-vr-helper crash-2026-05-02-02-24-32, rax=0 inside
			//   ImGui::NewFrame called from HUDDemo::Render.
			//
			// ImGui_ImplWin32_WndProcHandler reads BackendPlatformUserData
			// from the current context. If GImGui is settings_ctx (Win32
			// backend installed), input events are recorded there. If
			// GImGui is hud_ctx or anything else without a Win32
			// backend, GetBackendData returns null and the handler is a
			// no-op — we drop input events that fire during the brief
			// non-settings-context window. Acceptable: that's a few
			// frames a session, all input that matters arrives while
			// the render thread is in SettingsUI::Render with
			// settings_ctx active.
			ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
			return CallWindowProcA(g_origWndProc, hwnd, msg, wp, lp);
		}

		void InstallWndProcHook()
		{
			if (g_origWndProc || !g_hwnd)
				return;
			g_origWndProc = reinterpret_cast<WNDPROC>(
				SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC,
					reinterpret_cast<LONG_PTR>(WndProcThunk)));
			if (g_origWndProc) {
				logs::info("SettingsUI: WndProc hook installed (hwnd={})",
					reinterpret_cast<void*>(g_hwnd));
			} else {
				logs::warn("SettingsUI: SetWindowLongPtrA returned null; keyboard/mouse may not reach ImGui");
			}
		}

		// Primary action, shown at the top of the window: which registered
		// client's overlay is displayed in VR right now. This is the helper's
		// "launcher" — picking a client hands it the in-scene focus
		// (RequestFocus) so the user switches mods from here with no per-mod
		// controller combo, then closes this panel so the focus reconciler
		// doesn't immediately reclaim focus for the self-client.
		void RenderActiveOverlaySection()
		{
			namespace API = ImGuiVRHelperPluginAPI;
			auto& impl = HelperImpl::GetSingleton();
			const auto clients = impl.SnapshotClients();
			const uint32_t focusedId = impl.GetFocusedClientId();
			const uint32_t selfClientId = impl.GetSelfClientId();

			std::string preview = "(this) ImGuiVRHelper";
			if (focusedId != 0 && focusedId != selfClientId) {
				for (const auto& c : clients) {
					if (c.client_id == focusedId) {
						preview = c.name;
						if (!c.version.empty())
							preview += " " + c.version;
						break;
					}
				}
			}

			ImGui::TextUnformatted("Active overlay (shown in VR)");
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::BeginCombo("##ActiveOverlay", preview.c_str())) {
				for (const auto& c : clients) {
					if (c.client_id == selfClientId)
						continue;  // the helper itself is the current panel
					if (c.flags & API::kClientFlag_HUDMode)
						continue;  // HUD clients are always-on, not switchable
					std::string label = c.name;
					if (!c.version.empty())
						label += " " + c.version;
					if (!(c.flags & API::kClientFlag_RendersOnFocus))
						label += "  (manual trigger)";
					if (ImGui::Selectable(label.c_str(), c.client_id == focusedId)) {
						impl.RequestFocus(c.client_id);
						g_visible = false;  // yield; reconciler hands focus to the client
					}
				}
				ImGui::EndCombo();
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Pick which mod's overlay shows in VR. Selecting one closes this panel.");

			if (ImGui::Button("< Prev"))
				impl.CycleOverlay(-1);
			ImGui::SameLine();
			if (ImGui::Button("Next >"))
				impl.CycleOverlay(1);
			ImGui::SameLine();
			ImGui::TextDisabled("(or, off-panel: left stick-click = prev, right = next)");
		}

		void RenderPositioningSection(Overlay::Settings& s)
		{
			if (ImGui::CollapsingHeader("Overlay placement", ImGuiTreeNodeFlags_DefaultOpen)) {
				const char* attachLabels[] = { "HMD only", "Controller only", "Both", "None" };
				int attachIndex = static_cast<int>(s.attachMode);
				if (ImGui::Combo("Attach mode", &attachIndex, attachLabels, IM_ARRAYSIZE(attachLabels))) {
					s.attachMode = static_cast<Overlay::AttachMode>(attachIndex);
				}
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Where the overlay anchors: to the headset, a controller, both, or none.");

				const char* methodLabels[] = { "HMD-relative", "Fixed in world" };
				int methodIndex = static_cast<int>(s.positioningMethod);
				// Label deliberately differs from the enclosing
				// CollapsingHeader("Positioning") — ImGui hashes the
				// label into a widget ID, and two widgets in the same
				// window with the same label collide.
				if (ImGui::Combo("Positioning method", &methodIndex, methodLabels, IM_ARRAYSIZE(methodLabels))) {
					s.positioningMethod = static_cast<Overlay::PositioningMethod>(methodIndex);
				}
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("HMD-relative follows your head; Fixed places it in the room and stays put.");

				ImGui::SliderFloat("Panel size", &s.menuScale,
					Overlay::Config::kMinMenuScale, Overlay::Config::kMaxMenuScale, "%.2fx");
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Relative size of the overlay panel.");

				ImGui::Spacing();
				ImGui::TextDisabled("Reposition in VR: hold grip on a controller and move your hand.");
				ImGui::TextDisabled("Push that hand's thumbstick up/down to move it farther/closer.");
				ImGui::TextDisabled("(Toggle under Interaction > Grip-to-drag repositioning.)");
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
		}

		// Live WYSIWYG preview of the helper-drawn pointer over a mock panel, so
		// size/color changes are obvious without leaving the settings menu. Drawn
		// via ImGui's own draw list (not the real cursor texture) so it stays in
		// sync with the sliders without a GPU texture regen round-trip.
		void RenderCursorPreview(const Overlay::Settings& s)
		{
			constexpr float kBoxSize = 96.0f;
			constexpr ImU32 kPreviewBgColor = IM_COL32(60, 60, 65, 255);
			constexpr float kPreviewBgRounding = 4.0f;
			constexpr float kOutlineAlphaScale = 235.0f;
			constexpr float kArrowArmRatio = 0.34f;
			constexpr float kOutlinePolyThickness = 1.5f;
			constexpr float kCircleOuterRatio = 0.42f;
			constexpr float kCircleInnerRatio = 0.34f;

			ImGui::Dummy(ImVec2(0.0f, 4.0f));
			ImGui::TextDisabled("Preview");
			ImGui::InvisibleButton("##cursor_preview_area", ImVec2(kBoxSize, kBoxSize));
			ImDrawList* dl = ImGui::GetWindowDrawList();
			const ImVec2 origin = ImGui::GetItemRectMin();
			const ImVec2 size = ImGui::GetItemRectSize();
			const ImVec2 center(origin.x + size.x * 0.5f, origin.y + size.y * 0.5f);

			dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y),
				kPreviewBgColor, kPreviewBgRounding);

			const ImU32 fillColor = ImGui::ColorConvertFloat4ToU32(
				ImVec4(s.cursorColor[0], s.cursorColor[1], s.cursorColor[2], s.cursorColor[3]));
			const ImU32 outlineColor = IM_COL32(0, 0, 0, static_cast<int>(kOutlineAlphaScale * s.cursorColor[3]));
			const float scale = std::clamp(s.cursorSize, 0.5f, 3.0f) / 3.0f;

			if (s.cursorStyle == Overlay::CursorStyle::Arrow) {
				constexpr ImVec2 kPoly[7] = {
					{ 0.00f, 0.00f }, { 0.00f, 1.00f }, { 0.28f, 0.73f }, { 0.46f, 1.10f },
					{ 0.66f, 1.02f }, { 0.40f, 0.62f }, { 0.72f, 0.55f }
				};
				const float armLen = kBoxSize * kArrowArmRatio * scale;
				ImVec2 pts[7];
				for (int i = 0; i < 7; ++i)
					pts[i] = ImVec2(center.x + kPoly[i].x * armLen, center.y + kPoly[i].y * armLen);
				dl->AddConvexPolyFilled(pts, 7, fillColor);
				dl->AddPolyline(pts, 7, outlineColor, ImDrawFlags_Closed, kOutlinePolyThickness);
			} else {
				const float outerR = kBoxSize * kCircleOuterRatio * scale;
				const float innerR = kBoxSize * kCircleInnerRatio * scale;
				dl->AddCircleFilled(center, outerR, outlineColor);
				dl->AddCircleFilled(center, innerR, fillColor);
			}
		}

		void RenderInteractionSection(Overlay::Settings& s)
		{
			if (ImGui::CollapsingHeader("Interaction")) {
				ImGui::Checkbox("Wand pointing", &s.enableWandPointing);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Aim a controller at the panel to move the cursor.");
				// Style of the helper-drawn pointer (for clients that don't draw their own).
				int cursorStyle = static_cast<int>(s.cursorStyle);
				if (ImGui::Combo("Pointer style", &cursorStyle, "Dot\0Arrow\0"))
					s.cursorStyle = static_cast<Overlay::CursorStyle>(cursorStyle);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Shape of the wand pointer the helper draws over menus.");
				ImGui::SliderFloat("Pointer size", &s.cursorSize, 0.5f, 3.0f, "%.2fx");
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Scale of the helper-drawn pointer. 1.0 = default.");
				ImGui::ColorEdit4("Pointer color", s.cursorColor);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Tint of the helper-drawn pointer's fill; its outline stays dark for contrast.");
				RenderCursorPreview(s);
				ImGui::Checkbox("Grip-to-drag repositioning", &s.enableDragToReposition);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Hold grip and move your hand to reposition the overlay;\nthumbstick up/down moves it farther/closer.");
				ImGui::Checkbox("Only open overlays while the game is paused", &s.onlyOpenWhilePaused);
				ImGui::Checkbox("Show welcome banner on startup", &s.showWelcome);
				ImGui::SliderFloat("Thumbstick deadzone", &s.mouseDeadzone, 0.0f, 1.0f, "%.2f");
				ImGui::SliderFloat("Auto-reset distance",
					&s.autoResetDistance, 0.0f, 5000.0f, "%.0f units");
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Walk this far from a world-fixed overlay and it snaps back in\nfront of you. ~70 game units = 1 m. 0 = never.");

				ImGui::Separator();
				ImGui::TextDisabled("Two separate menus:");

				ImGui::BulletText("This settings panel");
				ImGui::TextDisabled("    Opens with Shift+F4, or a controller combo:");
				if (ImGui::Button("Rebind settings combo")) {
					auto& impl = HelperImpl::GetSingleton();
					ComboRecording::Begin(impl.GetSelfClientId(), "ImGuiVRHelper toggle", +[](const ImGuiVRHelperPluginAPI::InputCombo* keys, std::size_t n, void*) {
							if (n == 0) return;  // cancelled / timed out empty
							HelperImpl::GetSingleton().RebindSelfToggle(keys, n); }, nullptr, 5.0f);
				}

				ImGui::Spacing();
				ImGui::BulletText("Open / switch a mod overlay");
				ImGui::TextDisabled("    Brings up the active mod's menu (the launcher up top picks which).");
				if (ImGui::Button("Rebind open combo")) {
					auto& impl = HelperImpl::GetSingleton();
					ComboRecording::Begin(impl.GetSelfClientId(), "Open mod overlay", +[](const ImGuiVRHelperPluginAPI::InputCombo* keys, std::size_t n, void*) {
							if (n == 0) return;
							HelperImpl::GetSingleton().RebindOpenMenu(keys, n); }, nullptr, 5.0f);
				}
				ImGui::SameLine();
				if (ImGui::Button("Rebind close combo")) {
					auto& impl = HelperImpl::GetSingleton();
					ComboRecording::Begin(impl.GetSelfClientId(), "Close mod overlay", +[](const ImGuiVRHelperPluginAPI::InputCombo* keys, std::size_t n, void*) {
							if (n == 0) return;
							HelperImpl::GetSingleton().RebindCloseMenu(keys, n); }, nullptr, 5.0f);
				}
			}
		}

		void RenderClientsSection()
		{
			{  // own tab now; no wrapping collapsing header
				auto clients = HelperImpl::GetSingleton().SnapshotClients();
				ImGui::Text("Active clients: %zu", clients.size());

				// Overlay switching is handled by RenderActiveOverlaySection at
				// the top of the window; this section is the client roster.
				ImGui::Spacing();

				// Stable column identifiers — referenced by sort specs and
				// kept independent of column display order so reordering
				// columns in-place doesn't shuffle sort meaning.
				enum ClientsTableColumn : ImGuiID
				{
					kCol_ID = 0,
					kCol_Name = 1,
					kCol_Version = 2,
					kCol_Mode = 3,
					kCol_State = 4,
				};

				constexpr ImGuiTableFlags kTableFlags =
					ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
					ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Sortable |
					ImGuiTableFlags_SortMulti | ImGuiTableFlags_Resizable |
					ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
					ImGuiTableFlags_ContextMenuInBody | ImGuiTableFlags_ScrollY;

				// Cap height so a long client list paginates rather than
				// pushing the rest of the settings panel offscreen.
				const ImVec2 outerSize(0.0f, ImGui::GetTextLineHeightWithSpacing() * 12.0f);

				if (ImGui::BeginTable("##ClientsTable", 5, kTableFlags, outerSize)) {
					ImGui::TableSetupScrollFreeze(0, 1);  // pin header row
					ImGui::TableSetupColumn("ID",
						ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort |
							ImGuiTableColumnFlags_PreferSortAscending,
						50.0f, kCol_ID);
					ImGui::TableSetupColumn("Name",
						ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_PreferSortAscending,
						0.0f, kCol_Name);
					ImGui::TableSetupColumn("Version",
						ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortAscending,
						100.0f, kCol_Version);
					ImGui::TableSetupColumn("Mode",
						ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortAscending,
						110.0f, kCol_Mode);
					ImGui::TableSetupColumn("State",
						ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending,
						130.0f, kCol_State);
					ImGui::TableHeadersRow();

					namespace API = ImGuiVRHelperPluginAPI;

					// State sort key: focus > allocated > idle (so descending
					// brings interesting clients to the top).
					auto stateRank = [](const HelperImpl::ClientSnapshot& c) -> int {
						if (c.has_focus)
							return 2;
						if (c.has_texture)
							return 1;
						return 0;
					};
					// Mode sort key: Panel < HUD; +focus extends rank so
					// focus-required clients sort distinctly within their mode.
					auto modeRank = [](const HelperImpl::ClientSnapshot& c) -> int {
						int r = (c.flags & API::kClientFlag_HUDMode) ? 2 : 0;
						if (c.flags & API::kClientFlag_RequiresFocus)
							r += 1;
						return r;
					};

					if (auto* specs = ImGui::TableGetSortSpecs();
						specs && clients.size() > 1 &&
						(specs->SpecsDirty || specs->SpecsCount > 0)) {
						std::stable_sort(clients.begin(), clients.end(),
							[&](const HelperImpl::ClientSnapshot& a,
								const HelperImpl::ClientSnapshot& b) {
								for (int i = 0; i < specs->SpecsCount; ++i) {
									const ImGuiTableColumnSortSpecs& s = specs->Specs[i];
									int cmp = 0;
									switch (s.ColumnUserID) {
									case kCol_ID:
										cmp = (a.client_id < b.client_id) ? -1 : (a.client_id > b.client_id) ? 1 :
									                                                                           0;
										break;
									case kCol_Name:
										cmp = a.name.compare(b.name);
										break;
									case kCol_Version:
										cmp = a.version.compare(b.version);
										break;
									case kCol_Mode:
										{
											int ra = modeRank(a), rb = modeRank(b);
											cmp = (ra < rb) ? -1 : (ra > rb) ? 1 :
										                                       0;
										}
										break;
									case kCol_State:
										{
											int ra = stateRank(a), rb = stateRank(b);
											cmp = (ra < rb) ? -1 : (ra > rb) ? 1 :
										                                       0;
										}
										break;
									default:
										break;
									}
									if (cmp != 0)
										return s.SortDirection == ImGuiSortDirection_Ascending ? cmp < 0 : cmp > 0;
								}
								return false;
							});
						specs->SpecsDirty = false;
					}

					for (const auto& c : clients) {
						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						ImGui::Text("%u", c.client_id);
						ImGui::TableNextColumn();
						ImGui::TextUnformatted(c.name.c_str());
						ImGui::TableNextColumn();
						if (c.version.empty()) {
							ImGui::TextDisabled("-");
						} else {
							ImGui::TextUnformatted(c.version.c_str());
						}
						ImGui::TableNextColumn();
						if (c.flags & API::kClientFlag_HUDMode) {
							ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "HUD");
						} else {
							ImGui::TextColored(ImVec4(0.7f, 1.0f, 0.4f, 1.0f), "Panel");
						}
						if (c.flags & API::kClientFlag_RequiresFocus) {
							ImGui::SameLine();
							ImGui::TextDisabled("+focus");
						}
						ImGui::TableNextColumn();
						if (c.has_focus) {
							ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "FOCUS");
							ImGui::SameLine();
						}
						if (c.has_texture) {
							ImGui::TextDisabled("alloc");
						} else {
							ImGui::TextDisabled("idle");
						}
					}
					ImGui::EndTable();
				}
				ImGui::Spacing();
				ImGui::TextDisabled("Mode badge:  Panel = 3D quad in space");
				ImGui::TextDisabled("             HUD   = full-viewport on both eyes");
				ImGui::TextDisabled("State badge: FOCUS = receives input + drives 3D quad");
				ImGui::TextDisabled("             alloc = panel RTV created");
				ImGui::TextDisabled("             idle  = no RTV yet (lazy)");
			}
		}

		// Controller map: every registered combo across all clients, grouped by
		// client, each key color-coded by its controller (Theme::DeviceColor) and
		// clashes flagged. View-only for now; rebinding is the next step.
		void RenderControllerMapSection()
		{
			if (!ImGui::CollapsingHeader("Controller map"))
				return;
			namespace API = ImGuiVRHelperPluginAPI;
			const auto combos = HelperImpl::GetSingleton().SnapshotCombos();
			if (combos.empty()) {
				ImGui::TextDisabled("    No controller combos registered yet.");
				return;
			}
			uint32_t lastClient = 0xFFFFFFFFu;
			for (const auto& c : combos) {
				if (c.client_id != lastClient) {
					lastClient = c.client_id;
					ImGui::SeparatorText(c.client_name.empty() ? "(client)" : c.client_name.c_str());
				}
				ImGui::PushID(static_cast<int>(c.combo_id));
				ImGui::TextUnformatted(c.label.empty() ? "(unnamed)" : c.label.c_str());
				ImGui::SameLine(260.0f);
				for (size_t i = 0; i < c.keys.size(); ++i) {
					if (i != 0) {
						ImGui::SameLine(0.0f, 0.0f);
						ImGui::TextDisabled(" + ");
						ImGui::SameLine(0.0f, 0.0f);
					}
					ImGui::TextColored(Theme::DeviceColor(c.keys[i].GetDevice()), "%s",
						Input::ButtonName(c.keys[i].GetKey()));
				}
				if (c.off_panel) {
					ImGui::SameLine();
					ImGui::TextDisabled("  (off-panel)");
				}
				if (c.conflict) {
					ImGui::SameLine();
					ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "  [conflict]");
				}
				// Rebind reuses the same combo-recording modal as the self-toggle.
				// The combo_id rides through as `user`; RebindCombo updates it in
				// place (same id, so the client keeps polling its handle).
				ImGui::SameLine();
				if (ImGui::SmallButton("Rebind")) {
					ComboRecording::Begin(c.client_id, c.label.c_str(), +[](const API::InputCombo* keys, std::size_t n, void* user) {
							if (n == 0)
								return;  // cancelled / timed out
							HelperImpl::GetSingleton().RebindCombo(
								static_cast<API::ComboId>(reinterpret_cast<uintptr_t>(user)), keys, n); }, reinterpret_cast<void*>(static_cast<uintptr_t>(c.combo_id)), 5.0f);
				}
				// Clear (unbind). Reset isn't offered here — the helper doesn't
				// know a client's factory defaults; clients expose Reset via their
				// own bindings table (the SDK's DrawBindingsTable).
				if (!c.keys.empty()) {
					ImGui::SameLine();
					if (ImGui::SmallButton("Clear"))
						HelperImpl::GetSingleton().RebindCombo(c.combo_id, nullptr, 0);
				}
				ImGui::PopID();
			}
			ImGui::Spacing();
			ImGui::TextDisabled("    Key color = controller:");
			ImGui::SameLine();
			ImGui::TextColored(Theme::DeviceColor(API::InputDeviceType::Primary), "Primary");
			ImGui::SameLine(0.0f, 0.0f);
			ImGui::TextDisabled(",");
			ImGui::SameLine();
			ImGui::TextColored(Theme::DeviceColor(API::InputDeviceType::Secondary), "Secondary");
			ImGui::SameLine(0.0f, 0.0f);
			ImGui::TextDisabled(",");
			ImGui::SameLine();
			ImGui::TextColored(Theme::DeviceColor(API::InputDeviceType::Both), "Both");
		}

		// Order mod overlays for the open combo's "first mod" pick and the cycle.
		// The helper's own UI isn't reorderable — it has its own Shift+F4 toggle
		// and always leads the cycle.
		void RenderOverlayOrderSection()
		{
			if (!ImGui::CollapsingHeader("Overlay order"))
				return;
			auto& impl = HelperImpl::GetSingleton();
			const uint32_t selfId = impl.GetSelfClientId();
			std::vector<std::pair<uint32_t, std::string>> mods;
			for (auto& entry : impl.BuildOverlayOrder()) {
				if (entry.first != selfId)
					mods.push_back(entry);
			}
			if (mods.empty()) {
				ImGui::TextDisabled("    No mod overlays registered.");
				return;
			}
			ImGui::TextDisabled("    Top = opened first by the open combo; also the cycle order.");
			int moveFrom = -1, moveTo = -1;
			for (int i = 0; i < static_cast<int>(mods.size()); ++i) {
				ImGui::PushID(i);
				ImGui::Text("%d. %s", i + 1, mods[i].second.c_str());
				ImGui::SameLine(240.0f);
				ImGui::BeginDisabled(i == 0);
				if (ImGui::ArrowButton("##up", ImGuiDir_Up)) {
					moveFrom = i;
					moveTo = i - 1;
				}
				ImGui::EndDisabled();
				ImGui::SameLine();
				ImGui::BeginDisabled(i == static_cast<int>(mods.size()) - 1);
				if (ImGui::ArrowButton("##down", ImGuiDir_Down)) {
					moveFrom = i;
					moveTo = i + 1;
				}
				ImGui::EndDisabled();
				ImGui::PopID();
			}
			if (moveFrom >= 0) {
				std::swap(mods[moveFrom], mods[moveTo]);
				std::vector<std::string> names;
				names.reserve(mods.size());
				for (auto& m : mods)
					names.push_back(m.second);
				Overlay::State::GetSingleton().settings.overlayOrder = std::move(names);
				Overlay::SaveSettings();
			}
		}

		// Visual thumbstick state: a crosshair box with a dot at (x, y) plus a
		// numeric readout. Ported from Community Shaders' DrawThumbstickColumn.
		void DrawThumbstickPad(const char* label, const RE::VRControllerState& cs, RE::ControllerRole role)
		{
			const float x = cs.thumbsticks[static_cast<size_t>(role)].x;
			const float y = cs.thumbsticks[static_cast<size_t>(role)].y;

			const ImVec2 padSize(80.0f, 80.0f);
			const ImVec2 cursor = ImGui::GetCursorScreenPos();
			ImDrawList* dl = ImGui::GetWindowDrawList();
			const ImVec2 center(cursor.x + padSize.x * 0.5f, cursor.y + padSize.y * 0.5f);
			const float radius = padSize.x * 0.5f - 4.0f;

			dl->AddRectFilled(cursor, ImVec2(cursor.x + padSize.x, cursor.y + padSize.y), ImGui::GetColorU32(ImGuiCol_FrameBg));
			dl->AddRect(cursor, ImVec2(cursor.x + padSize.x, cursor.y + padSize.y), ImGui::GetColorU32(ImGuiCol_Border), 4.0f, 0, 2.0f);
			const ImU32 axis = ImGui::GetColorU32(ImGuiCol_TextDisabled);
			dl->AddLine(ImVec2(center.x, cursor.y + 4), ImVec2(center.x, cursor.y + padSize.y - 4), axis, 1.0f);
			dl->AddLine(ImVec2(cursor.x + 4, center.y), ImVec2(cursor.x + padSize.x - 4, center.y), axis, 1.0f);
			dl->AddCircleFilled(ImVec2(center.x + x * radius, center.y - y * radius), 5.0f, ImGui::GetColorU32(ImGuiCol_Text));

			ImGui::Dummy(padSize);
			ImGui::Text("%s", label);
			ImGui::Text("X:% .2f Y:% .2f", x, y);
		}

		// Visual quality / placement — first-class settings, so on the Overlay tab
		// (not buried in Diagnostics). Base resolution + supersample apply at panel
		// allocation, hence the restart note; depth/coverage/toast are live.
		void RenderDisplaySection(Overlay::Settings& s)
		{
			ImGui::TextDisabled("HUD geometry");
			ImGui::SliderFloat("HUD depth (m)", &s.hudDepth, 0.5f, 3.0f, "%.2f");
			ImGui::SliderFloat("HUD coverage", &s.hudCoverage, Overlay::Config::kMinHUDCoverage,
				Overlay::Config::kMaxHUDCoverage, "%.2f");
			ImGui::TextDisabled("    Fraction of the view the HUD fills; 1.0 may clip at the lens.");

			ImGui::Spacing();
			ImGui::Separator();
			ImGui::TextDisabled("Sharpness");

			// Common monitor presets; "Custom" shows if the TOML holds another size.
			struct ResPreset
			{
				const char* label;
				int w;
				int h;
			};
			static const ResPreset kResPresets[] = {
				{ "1920 x 1080", 1920, 1080 },
				{ "2560 x 1440", 2560, 1440 },
				{ "3840 x 2160", 3840, 2160 },
			};
			int curRes = -1;
			for (int i = 0; i < IM_ARRAYSIZE(kResPresets); ++i)
				if (kResPresets[i].w == s.baseWidth && kResPresets[i].h == s.baseHeight) {
					curRes = i;
					break;
				}
			const char* resPreview = curRes >= 0 ? kResPresets[curRes].label : "Custom";
			if (ImGui::BeginCombo("Base resolution", resPreview)) {
				for (int i = 0; i < IM_ARRAYSIZE(kResPresets); ++i) {
					if (ImGui::Selectable(kResPresets[i].label, i == curRes)) {
						s.baseWidth = kResPresets[i].w;
						s.baseHeight = kResPresets[i].h;
					}
				}
				ImGui::EndCombo();
			}
			ImGui::SliderInt("HUD supersample", &s.hudSupersample, 1, Overlay::Config::kMaxHUDSupersample, "%dx");
			int baseW, baseH;
			ResolveBaseDims(baseW, baseH);
			const int ss = HudSupersample();
			const double panelMB =
				static_cast<double>(baseW) * baseH * ss * ss * 4.0 / (1024.0 * 1024.0);
			ImGui::TextDisabled("    %d x %d, ~%.0f MB VRAM each (HUD, settings, banners each allocate one)",
				baseW * ss, baseH * ss, panelMB);
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.1f, 1.0f),
				"    [restart required] base resolution + supersample apply after a Skyrim restart.");

			ImGui::Spacing();
			ImGui::Separator();
			ImGui::TextDisabled("Render path");
			ImGui::Checkbox("Native VR overlay for menus (experimental)", &s.useRuntimeOverlay);
			ImGui::TextDisabled(
				"    Composites the focused menu as a runtime overlay layer, immune to\n"
				"    upscalers and frame generation. Falls back to in-scene rendering\n"
				"    when the runtime has no overlay support.");

			ImGui::Spacing();
			ImGui::Separator();
			ImGui::TextDisabled("Banners");
			ImGui::SliderFloat("Toast position", &s.toastTopFraction, 0.0f, 1.0f, "%.2f");
			ImGui::TextDisabled("    Vertical placement of welcome/swap banners (0=top, 1=bottom).");
		}

		void RenderDiagnosticsSection(Overlay::State& state)
		{
			auto& s = state.settings;
			{  // own tab now; no wrapping collapsing header
				// OpenVR runtime — detected once at startup (VRDetection::Detect),
				// cached for display here.
				const auto& vrInfo = VRDetection::LastResult();
				ImGui::TextDisabled("OpenVR runtime");
				if (vrInfo.isAvailable) {
					// Interface availability is queried live from the game's
					// BSOpenVR: the startup probe runs at kPostPostLoad, before VR
					// is up, so it always reported "missing". Fetching the cached
					// interface pointers is render-thread-safe — only calling into
					// them off the input thread is not. IVROverlay is omitted: it's
					// context-bound and unused by the in-scene/HUD path.
					auto* openvr = RE::BSOpenVR::GetSingleton();
					const bool sysOk = openvr && RE::BSOpenVR::GetIVRSystem() != nullptr;
					const bool compOk = openvr && RE::BSOpenVR::GetIVRCompositor() != nullptr;
					if (sysOk && compOk)
						ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Active & compatible");
					else
						ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "Detected; interfaces not ready");
					ImGui::Text("Runtime: %s", VRDetection::RuntimeTypeToString(vrInfo.runtimeType));
					if (!vrInfo.dllPath.empty())
						ImGui::Text("DLL: %s", vrInfo.dllPath.c_str());
					ImGui::Text("Version: %s   Size: %llu bytes",
						vrInfo.version.c_str(), static_cast<unsigned long long>(vrInfo.fileSize));
					if (!vrInfo.modificationTime.empty())
						ImGui::Text("Modified: %s", vrInfo.modificationTime.c_str());
					ImGui::Text("Interfaces:  System %s   Compositor %s",
						sysOk ? "OK" : "missing", compOk ? "OK" : "missing");
				} else {
					ImGui::TextDisabled("OpenVR not available");
				}
				ImGui::Spacing();
				ImGui::Separator();

				ImGui::Text("Overlay visible: %s", state.overlayVisible ? "yes" : "no");
				ImGui::Text("Left-handed: %s", state.lastKnownLeftHandedMode ? "yes" : "no");
				ImGui::Text("Wand intersecting: %s",
					state.wandState.isIntersecting ? "yes" : "no");
				if (state.wandState.isIntersecting) {
					ImGui::Text("  UV: (%.3f, %.3f)",
						state.wandState.uvCoordinatesX.load(std::memory_order_relaxed),
						state.wandState.uvCoordinatesY.load(std::memory_order_relaxed));
				}
				ImGui::Text("Drag active: %s", state.dragState.dragging.load(std::memory_order_relaxed) ? "yes" : "no");

				// VR controller input diagnostics — migrated from Community
				// Shaders' VR debug page. The helper owns the authoritative input
				// state (it drives wand pointing, combos, and focus), so a client's
				// own debug page would only see what it forwards; this is the place
				// to inspect the real per-controller state.
				ImGui::Spacing();
				ImGui::Separator();
				ImGui::TextDisabled("VR controller input (live)");
				ImGui::Text("Focused client: %u   Wand: %s",
					HelperImpl::GetSingleton().GetFocusedClientId(),
					state.wandState.isIntersecting ? "intersecting" : "none");
				if (state.wandState.isIntersecting) {
					ImGui::Text("    Wand UV (%.3f, %.3f), device %u",
						state.wandState.uvCoordinatesX.load(std::memory_order_relaxed),
						state.wandState.uvCoordinatesY.load(std::memory_order_relaxed),
						state.wandState.controllerIndex);
				}

				const bool leftHanded = state.lastKnownLeftHandedMode;
				const double nowSecs = std::chrono::duration<double>(
					std::chrono::steady_clock::now().time_since_epoch())
				                           .count();

				{
					const ImU32 pressedBg = ImGui::GetColorU32(ImVec4(0.20f, 0.45f, 0.30f, 0.55f));
					if (ImGui::BeginTable("##vrButtonState", 5,
							ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
						ImGui::TableSetupColumn("Button");
						ImGui::TableSetupColumn(leftHanded ? "Primary" : "Secondary");
						ImGui::TableSetupColumn("Held s##a");
						ImGui::TableSetupColumn(leftHanded ? "Secondary" : "Primary");
						ImGui::TableSetupColumn("Held s##b");
						ImGui::TableHeadersRow();

						auto stateCell = [&](const RE::ButtonState& b) {
							if (b.isPressed)
								ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, pressedBg);
							ImGui::TextUnformatted(b.isPressed ? "Pressed" : "-");
						};
						auto heldCell = [&](const RE::ButtonState& b) {
							if (b.isPressed)
								ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, pressedBg);
							ImGui::Text("%.2f", b.GetCurrentHeldTime(nowSecs));
						};
						for (const auto& db : Input::ButtonTable()) {
							if (!db.diagnostics)
								continue;
							const RE::ButtonState& pri = state.primaryControllerState[db.reKey];
							const RE::ButtonState& sec = state.secondaryControllerState[db.reKey];
							const RE::ButtonState& a = leftHanded ? pri : sec;
							const RE::ButtonState& b = leftHanded ? sec : pri;
							ImGui::TableNextRow();
							ImGui::TableSetColumnIndex(0);
							ImGui::TextUnformatted(db.name);
							ImGui::TableSetColumnIndex(1);
							stateCell(a);
							ImGui::TableSetColumnIndex(2);
							heldCell(a);
							ImGui::TableSetColumnIndex(3);
							stateCell(b);
							ImGui::TableSetColumnIndex(4);
							heldCell(b);
						}
						ImGui::EndTable();
					}
				}

				// Thumbstick visualizers (handedness-ordered).
				if (ImGui::BeginTable("##vrSticks", 2, ImGuiTableFlags_SizingFixedFit)) {
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::BeginGroup();
					DrawThumbstickPad(leftHanded ? "Primary" : "Secondary",
						leftHanded ? state.primaryControllerState : state.secondaryControllerState,
						leftHanded ? RE::ControllerRole::Primary : RE::ControllerRole::Secondary);
					ImGui::EndGroup();
					ImGui::TableSetColumnIndex(1);
					ImGui::BeginGroup();
					DrawThumbstickPad(leftHanded ? "Secondary" : "Primary",
						leftHanded ? state.secondaryControllerState : state.primaryControllerState,
						leftHanded ? RE::ControllerRole::Secondary : RE::ControllerRole::Primary);
					ImGui::EndGroup();
					ImGui::EndTable();
				}

				// Recent VR controller events (most recent first).
				ImGui::SeparatorText("Recent VR controller events");
				if (ImGui::BeginTable("##vrEvents", 5,
						ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY,
						ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 8.0f))) {
					ImGui::TableSetupColumn("Device", ImGuiTableColumnFlags_WidthFixed, 60.0f);
					ImGui::TableSetupColumn("Key / X", ImGuiTableColumnFlags_WidthFixed, 80.0f);
					ImGui::TableSetupColumn("Y", ImGuiTableColumnFlags_WidthFixed, 70.0f);
					ImGui::TableSetupColumn("Pressed", ImGuiTableColumnFlags_WidthFixed, 70.0f);
					ImGui::TableSetupColumn("Mapping", ImGuiTableColumnFlags_WidthStretch);
					ImGui::TableHeadersRow();
					const auto events = ImGuiVRHelper::Input::SnapshotEventLog();
					for (auto it = events.rbegin(); it != events.rend(); ++it) {
						const auto& e = *it;
						ImGui::TableNextRow();
						ImGui::TableSetColumnIndex(0);
						ImGui::Text("%u", e.device);
						ImGui::TableSetColumnIndex(1);
						if (e.isThumbstick)
							ImGui::Text("%.3f", e.thumbstickX);
						else
							ImGui::Text("%u", e.keyCode);
						ImGui::TableSetColumnIndex(2);
						if (e.isThumbstick)
							ImGui::Text("%.3f", e.thumbstickY);
						else
							ImGui::TextUnformatted("-");
						ImGui::TableSetColumnIndex(3);
						ImGui::TextUnformatted(e.pressed ? "yes" : "no");
						ImGui::TableSetColumnIndex(4);
						ImGui::TextUnformatted(e.isThumbstick ? "Thumbstick" : Input::ButtonName(e.keyCode));
					}
					ImGui::EndTable();
				}

				ImGui::Spacing();
				ImGui::Separator();
				ImGui::TextDisabled("HUD-mode smoke test");
				ImGui::Checkbox("Show HUD demo", &s.showHUDDemo);
				if (s.showHUDDemo) {
					ImGui::TextDisabled("    A calibration grid covers the HUD layer.");
					ImGui::TextDisabled("    If lines + center crosshair are visible:");
					ImGui::TextDisabled("    kClientFlag_HUDMode is wired correctly.");
				}

				ImGui::Spacing();
				ImGui::Separator();
				ImGui::TextDisabled("HUD layers (debug) — always-on, drawn in registration order");
				{
					namespace API = ImGuiVRHelperPluginAPI;
					auto& impl = HelperImpl::GetSingleton();
					const auto clients = impl.SnapshotClients();
					int hudCount = 0;
					for (const auto& c : clients) {
						if (!(c.flags & API::kClientFlag_HUDMode))
							continue;
						++hudCount;
						// Checkbox = "enabled"; unchecking force-disables the layer
						// so a developer can isolate who draws what / spot overlaps.
						bool enabled = !c.hud_force_disabled;
						const std::string label = c.name + "##hud" + std::to_string(c.client_id);
						if (ImGui::Checkbox(label.c_str(), &enabled))
							impl.SetHudForceDisabled(c.client_id, !enabled);
						ImGui::SameLine();
						ImGui::TextColored(
							c.hud_compositing ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
							c.hud_force_disabled ? "disabled" : (c.hud_compositing ? "compositing" : "idle (no cost)"));
					}
					if (hudCount == 0)
						ImGui::TextDisabled("    (no HUD-mode clients registered)");
					else
						ImGui::TextDisabled("    Uncheck to hide a layer; 'idle' layers are skipped (zero cost).");
				}
			}
		}

		void RenderWindow()
		{
			auto& state = Overlay::State::GetSingleton();
			auto& s = state.settings;

			// Fill the whole panel by default so the settings occupy the full texture
			// rather than floating as a small window in an otherwise-empty quad. Still
			// movable/resizable within the panel for the session (no ini persistence).
			ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_FirstUseEver);
			// Title shows the version; "###" keeps a stable window ID across versions.
			if (!ImGui::Begin("ImGuiVRHelper Settings  v" IMGUI_VR_HELPER_VERSION_STRING "###ImGuiVRHelperSettings",
					&g_visible)) {
				ImGui::End();
				return;
			}

			// Clamp the window inside the 1920x1080 panel each frame.
			// ImGui doesn't keep windows fully on-screen by default — the
			// user could drag this off the edge and lose half of it
			// behind the overlay's RTV bounds. We snap the position back
			// only when it's actually drifted out of bounds, so normal
			// drag inside the panel feels natural.
			{
				const ImVec2 winPos = ImGui::GetWindowPos();
				const ImVec2 winSize = ImGui::GetWindowSize();
				const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
				const ImVec2 clamped(
					std::clamp(winPos.x, 0.0f, std::max(0.0f, displaySize.x - winSize.x)),
					std::clamp(winPos.y, 0.0f, std::max(0.0f, displaySize.y - winSize.y)));
				if (clamped.x != winPos.x || clamped.y != winPos.y) {
					ImGui::SetWindowPos(clamped);
				}
			}

			RenderActiveOverlaySection();  // primary action, on top
			ImGui::Separator();

			// Grouped tabs: each holds one topic so a tab never wraps its whole
			// body in a redundant collapsing header, and the live per-frame
			// Diagnostics readouts stay off the settings pages (their changing
			// height would otherwise scroll the controls up and down).
			if (ImGui::BeginTabBar("##HelperTabs")) {
				if (ImGui::BeginTabItem("Overlay")) {
					RenderPositioningSection(s);
					ImGui::Separator();
					RenderDisplaySection(s);
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Controls")) {
					RenderInteractionSection(s);
					RenderControllerMapSection();
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Clients")) {
					RenderClientsSection();
					ImGui::Separator();
					RenderOverlayOrderSection();  // "which mods, in what order" lives with the roster
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Diagnostics")) {
					RenderDiagnosticsSection(state);
					ImGui::EndTabItem();
				}
				ImGui::EndTabBar();
			}

			// Footer: reset stays reachable from any tab. Confirm first — a stray
			// wand click here would otherwise wipe key bindings and overlay order.
			ImGui::Separator();
			if (ImGui::Button("Reset all settings to defaults"))
				ImGui::OpenPopup("Reset all settings?");
			if (ImGui::BeginPopupModal("Reset all settings?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
				ImGui::TextUnformatted("Resets ALL helper settings to defaults — placement,");
				ImGui::TextUnformatted("controls, key bindings, overlay order, and display.");
				ImGui::Spacing();
				if (ImGui::Button("Reset everything")) {
					s = Overlay::Settings{};
					Overlay::ApplyLogLevel();  // logLevel is applied once, not per-frame
					Overlay::SaveSettings();
					logs::info("Settings reset to defaults");
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if (ImGui::Button("Cancel"))
					ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
			}

			ImGui::End();
		}

		void RenderQuickSelectMenu()
		{
			auto& impl = HelperImpl::GetSingleton();
			const auto order = impl.BuildOverlayOrder();
			if (order.empty())
				return;

			// Center the window on the 1920x1080 display
			const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
			ImGui::SetNextWindowPos(ImVec2(displaySize.x * 0.5f, displaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));

			// Auto-size to whatever the title/list/footer actually need at the current font size,
			// instead of a fixed 600x400 that assumed the old stretched-bitmap font -- with a real TTF
			// baked at a much larger native size, that box was too small the moment there was more than
			// a couple of overlay entries, clipping the list. Capped at 80% of the display height so an
			// unusually long client list scrolls (see the child region below) rather than growing the
			// window off-screen.
			ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, displaySize.y * 0.8f));

			ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;

			// Custom colors & styles: transparent dark background, clean borders, rounded corners, large text
			ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.02f, 0.02f, 0.02f, 0.90f));
			ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 3.0f);
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 16.0f));

			if (ImGui::Begin("##QuickSelect", nullptr, flags)) {
				// Title
				ImGui::SetWindowFontScale(1.8f);
				ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "  QUICK OVERLAY SELECT");
				ImGui::SetWindowFontScale(1.0f);
				ImGui::Separator();
				ImGui::Dummy(ImVec2(0.0f, 10.0f));

				const int hoveredIdx = impl.GetQuickSelectHoveredIdx();

				ImGui::SetWindowFontScale(1.4f);
				// Cap the visible list at ~10 rows (measured at the actual current font size, not a
				// guessed pixel count) and let it scroll past that -- covers an unusually long client
				// list without the whole window (and the footer below it) growing past the outer
				// height constraint.
				const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
				const float listHeight = std::min(lineHeight * static_cast<float>(order.size()), lineHeight * 10.0f);
				if (ImGui::BeginChild("##QuickSelectList", ImVec2(0.0f, listHeight))) {
					for (int i = 0; i < static_cast<int>(order.size()); ++i) {
						const bool isHovered = (i == hoveredIdx);
						const uint32_t clientId = order[i].first;
						const auto& clientName = order[i].second;
						const std::string version = impl.GetClientVersion(clientId);

						if (isHovered) {
							if (!version.empty()) {
								ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "  >  %s  (v%s)", clientName.c_str(), version.c_str());
							} else {
								ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "  >  %s", clientName.c_str());
							}
						} else {
							if (!version.empty()) {
								ImGui::Text("     %s  (v%s)", clientName.c_str(), version.c_str());
							} else {
								ImGui::Text("     %s", clientName.c_str());
							}
						}
					}
				}
				ImGui::EndChild();
				ImGui::SetWindowFontScale(1.0f);

				ImGui::Dummy(ImVec2(0.0f, 15.0f));
				ImGui::Separator();
				ImGui::SetWindowFontScale(1.1f);
				ImGui::TextDisabled("  Trigger / Stick Click to select. Grip to cancel.");
				ImGui::SetWindowFontScale(1.0f);
			}
			ImGui::End();

			ImGui::PopStyleVar(3);
			ImGui::PopStyleColor(2);
		}

		// Same fix client mods apply via the SDK's ApplyPanelDisplaySize: override
		// the Win32-backend-derived io.DisplaySize with the actual panel texture's
		// pixel dimensions, so the wand cursor (mapped from panel UV) and the
		// rendered content agree on the same canvas. No-op until the self client
		// (and its panel) exist.
		void ApplySelfPanelDisplaySize()
		{
			auto& helper = HelperImpl::GetSingleton();
			const uint32_t selfId = helper.GetSelfClientId();
			if (selfId == 0)
				return;
			ImGuiVRHelperPluginAPI::PanelHandle panel{};
			if (!helper.GetPanel(selfId, &panel) || !panel.width || !panel.height)
				return;
			ImGuiIO& io = ImGui::GetIO();
			io.DisplaySize = ImVec2(static_cast<float>(panel.width), static_cast<float>(panel.height));
			io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
		}
	}  // namespace

	bool Init()
	{
		if (g_ctx)
			return true;
		if (!Globals::IsReady())
			return false;

		g_ctx = CreateHudContext("SettingsUI");
		if (!g_ctx)
			return false;

		// Unlike the non-interactive HUD layers, the settings panel takes desktop
		// mouse + keyboard, so enable cursors + keyboard nav on its context.
		ImGuiIO& io = ImGui::GetIO();
		io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

		auto& d3d = Globals::GetD3D();

		// Win32 input backend. Hooks the swapchain window's WndProc so
		// the user's desktop mouse + keyboard reach this ImGui context
		// (slider input fields, copy/paste, ImGui keyboard nav). The
		// per-context backend data ImGui_ImplWin32 stores in the
		// context's BackendPlatformUserData makes this safe to coexist
		// with HUDDemo's separate context.
		DXGI_SWAP_CHAIN_DESC desc{};
		if (d3d.swapchain && SUCCEEDED(d3d.swapchain->GetDesc(&desc)) && desc.OutputWindow) {
			g_hwnd = desc.OutputWindow;
			if (!ImGui_ImplWin32_Init(g_hwnd)) {
				logs::warn("SettingsUI::Init: ImGui_ImplWin32_Init failed; keyboard/mouse will not work");
				g_hwnd = nullptr;
			} else {
				InstallWndProcHook();
			}
		} else {
			logs::warn("SettingsUI::Init: couldn't resolve swapchain HWND; keyboard/mouse disabled");
		}

		logs::info("SettingsUI initialized (helper ImGui context @ {})", static_cast<void*>(g_ctx));
		return true;
	}

	void Shutdown()
	{
		if (!g_ctx)
			return;
		ImGui::SetCurrentContext(g_ctx);
		// Restore the original WndProc before tearing down ImGui's
		// Win32 backend — the thunk closes over g_ctx and would
		// dereference a destroyed context if a stray message arrives
		// after Shutdown. Idempotent against not-yet-installed hook.
		if (g_origWndProc && g_hwnd) {
			SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC,
				reinterpret_cast<LONG_PTR>(g_origWndProc));
			g_origWndProc = nullptr;
		}
		g_hwnd = nullptr;
		ImGui_ImplWin32_Shutdown();
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

	bool IsVisible() { return g_visible || HelperImpl::GetSingleton().IsQuickSelectActive(); }

	// Programmatic visibility set (e.g. the focus reconciler auto-closing the
	// self-UI when a client takes focus). Like Toggle, this is a pure flag set;
	// HelperImpl's reconciler owns the focus/persist cleanup.
	void SetVisible(bool visible) { g_visible = visible; }

	ImGuiContext* GetContext() { return g_ctx; }

	bool Render(float dt)
	{
		if (!g_ctx)
			return false;
		// Render only when the settings window is up.
		// The rebind capture overlay renders independently — ComboRecording owns
		// its own context + panel and composites over the focused client.
		if (!g_visible && !HelperImpl::GetSingleton().IsQuickSelectActive())
			return false;

		ImGui::SetCurrentContext(g_ctx);

		ImGuiIO& io = ImGui::GetIO();
		io.DeltaTime = dt > 0.0f ? dt : 1.0f / 60.0f;

		if (HelperImpl::GetSingleton().IsQuickSelectActive()) {
			ImGui_ImplDX11_NewFrame();
			if (g_hwnd) {
				ImGui_ImplWin32_NewFrame();
			}
			ApplySelfPanelDisplaySize();
			ImGui::NewFrame();
			io.MouseDrawCursor = false;
			RenderQuickSelectMenu();
			ImGui::Render();
			return true;
		}

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

		// Backends FIRST, wand/stick injection second, so the injected cursor
		// wins over the Win32 backend's OS-cursor sample. The old order relied
		// on WantSetMousePos round-tripping the wand position through the OS
		// cursor in the game window's client area — Windows clamps the cursor
		// to that window, so any mirror window smaller than the canvas dragged
		// the readback toward the window edge, diverging from the wand (and
		// from the composited dot) worse toward the panel edges.
		ImGui_ImplDX11_NewFrame();
		// Win32 input backend's NewFrame captures cursor pos + key state
		// from Windows and feeds it into this context's IO. Skipped if
		// the WndProc hook didn't install (e.g. swapchain HWND wasn't
		// resolvable) — controllers still drive ImGui via the thumbstick
		// + button paths below, just without desktop input.
		if (g_hwnd) {
			ImGui_ImplWin32_NewFrame();
		}
		// ImGui_ImplWin32_NewFrame sets io.DisplaySize from the game window's
		// client rect (its native/mirror resolution) -- unrelated to the actual
		// panel texture this content renders into, which HelperImpl allocates
		// supersampled (PanelPixelSize: baseWidth/Height * hudSupersample for the
		// self client, since it fills the view like a HUD layer). Left alone, the
		// UI only fills a sub-rect of that texture sized to the window's native
		// resolution, and the wand cursor (mapped from panel UV, i.e. the FULL
		// supersampled dimensions) diverges by exactly the supersample factor.
		// Same fix client mods apply via the SDK's ApplyPanelDisplaySize.
		ApplySelfPanelDisplaySize();

		WandPointing::UpdateCursorFromWandPointing();

		// Per-thumbstick scroll accumulators. Mirrors upstream/dev's
		// `static std::unordered_map<size_t, ScrollAccum>` in
		// VR::ProcessThumbstickScroll — accumulator is keyed by stick
		// so two sticks scrolling simultaneously don't cross-contaminate.
		// Two sticks (primary/secondary) so two struct slots suffice.
		struct StickAccum
		{
			float x = 0.0f;
			float y = 0.0f;
		};
		static StickAccum primAccum;
		static StickAccum secAccum;

		auto processScroll = [&](const RE::VRControllerState& ctl,
								 size_t thumbIdx, StickAccum& accum) {
			const float sx = ctl.thumbsticks[thumbIdx].x;
			const float sy = ctl.thumbsticks[thumbIdx].y;
			if (std::abs(sx) > settings.mouseDeadzone)
				accum.x += sx * 0.1f;
			if (std::abs(sy) > settings.mouseDeadzone)
				accum.y += sy * 0.1f;
			float wheelX = 0.0f, wheelY = 0.0f;
			if (std::abs(accum.x) > 0.3f) {
				wheelX = accum.x > 0 ? 1.0f : -1.0f;
				accum.x = 0.0f;
			}
			if (std::abs(accum.y) > 0.3f) {
				wheelY = accum.y > 0 ? 1.0f : -1.0f;
				accum.y = 0.0f;
			}
			if (wheelX != 0.0f || wheelY != 0.0f) {
				// X is negated to match upstream/dev:Input.cpp:305
				// (`io.AddMouseWheelEvent(-scrollEventX, scrollEventY)`).
				// Right stick deflection pushes content LEFT in ImGui,
				// like a touchpad scroll gesture.
				io.AddMouseWheelEvent(-wheelX, wheelY);
			}
		};

		auto processCursor = [&](const RE::VRControllerState& ctl, size_t thumbIdx) {
			constexpr float kMouseSpeed = 12.0f;
			const float cx = ctl.thumbsticks[thumbIdx].x;
			const float cy = ctl.thumbsticks[thumbIdx].y;
			if (std::abs(cx) > settings.mouseDeadzone ||
				std::abs(cy) > settings.mouseDeadzone) {
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
		};

		// Branch on whether the wand laser is currently on the panel —
		// mirrors upstream/dev:Input.cpp:329-345's
		// `if (wandHandledCursor && !isDragging)` switch:
		//
		//   Wand on panel: BOTH thumbsticks scroll. The wand is already
		//                  driving the cursor (UpdateCursorFromWandPointing
		//                  ran above), so neither stick should fight it.
		//   Wand off panel: one stick drives the cursor, the other
		//                   drives scroll. Cursor stick is the OPPOSITE
		//                   hand of whichever the menu is attached to so
		//                   the user isn't reaching across.
		//
		// Drag mode disables both stick paths; OverlayDrag already owns
		// the cursor and scroll behavior would interfere.
		namespace API = ImGuiVRHelperPluginAPI;
		const bool isDragging = vrState.dragState.dragging.load(std::memory_order_relaxed);
		const bool wandHandledCursor = settings.enableWandPointing &&
		                               vrState.wandState.isIntersecting;

		if (wandHandledCursor && !isDragging) {
			processScroll(vrState.primaryControllerState,
				static_cast<size_t>(RE::ControllerRole::Primary), primAccum);
			processScroll(vrState.secondaryControllerState,
				static_cast<size_t>(RE::ControllerRole::Secondary), secAccum);
		} else if (!isDragging) {
			const bool menuOnPrimary =
				(settings.attachMode == Overlay::AttachMode::ControllerOnly &&
					settings.attachController == API::InputDeviceType::Primary);
			if (menuOnPrimary) {
				// Menu is on primary hand — secondary drives cursor,
				// primary drives scroll.
				processCursor(vrState.secondaryControllerState,
					static_cast<size_t>(RE::ControllerRole::Secondary));
				processScroll(vrState.primaryControllerState,
					static_cast<size_t>(RE::ControllerRole::Primary), primAccum);
			} else {
				processCursor(vrState.primaryControllerState,
					static_cast<size_t>(RE::ControllerRole::Primary));
				processScroll(vrState.secondaryControllerState,
					static_cast<size_t>(RE::ControllerRole::Secondary), secAccum);
			}
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
			// Stick-click is the off-panel overlay-cycle shortcut
			// (HelperImpl::ProcessOverlayCycleInput), not a mouse button.
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

		ImGui::NewFrame();

		io.MouseDrawCursor =
			io.MousePos.x >= 0.0f && io.MousePos.y >= 0.0f &&
			io.MousePos.x < io.DisplaySize.x && io.MousePos.y < io.DisplaySize.y;

		if (g_visible) {
			RenderWindow();
		}
		ImGui::Render();
		return true;
	}
}
