// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include "pch.h"

#include "SettingsUI.h"

#include "ComboRecording.h"
#include "Dashboard.h"
#include "Globals.h"
#include "HelperImpl.h"
#include "Overlay.h"
#include "WandPointing.h"

#include <algorithm>
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
		// Set by HelperImpl when the SteamVR dashboard is showing the
		// helper's panel: render the window even if the user never
		// toggled it open via the hotkey.
		bool g_forceVisible = false;

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

		void RenderWindow()
		{
			auto& state = Overlay::State::GetSingleton();
			auto& s = state.settings;

			ImGui::SetNextWindowSize(ImVec2(700, 600), ImGuiCond_FirstUseEver);
			if (!ImGui::Begin("ImGuiVRHelper Settings", &g_visible)) {
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
				ImGui::TextUnformatted("Toggle hotkey: Shift+F4 (keyboard).");
				ImGui::TextDisabled("Click below to bind a controller combo too.");
				if (ImGui::Button("Rebind toggle key")) {
					auto& impl = HelperImpl::GetSingleton();
					ComboRecording::Begin(impl.GetSelfClientId(), "ImGuiVRHelper toggle", +[](const ImGuiVRHelperPluginAPI::InputCombo* keys, std::size_t n, void* /*user*/) {
							if (n == 0) return;  // user cancelled or timed out empty
							HelperImpl::GetSingleton().RebindSelfToggle(keys, n); }, nullptr, 5.0f);
				}
			}

			if (ImGui::CollapsingHeader("Registered Clients")) {
				auto clients = HelperImpl::GetSingleton().SnapshotClients();
				ImGui::Text("Active clients: %zu", clients.size());

				// SteamVR Dashboard picker. The helper owns one shared
				// dashboard surface; this combo picks which eligible
				// client's panel texture is mirrored onto it. Selecting
				// "(self) ImGuiVRHelper" routes back to the helper's
				// own settings UI — useful when the user wants to
				// reconfigure the helper from the SteamVR dashboard
				// without leaving it.
				const uint32_t activePicker = Dashboard::GetActiveClient();
				const auto selfId = HelperImpl::GetSingleton().GetSelfClientId();
				std::string preview = "(self) ImGuiVRHelper";
				if (activePicker != 0) {
					for (const auto& c : clients) {
						if (c.client_id == activePicker) {
							preview = c.name;
							if (!c.version.empty())
								preview += " " + c.version;
							break;
						}
					}
				}

				ImGui::Spacing();
				ImGui::TextDisabled("SteamVR dashboard panel currently shows:");
				namespace API = ImGuiVRHelperPluginAPI;
				if (ImGui::BeginCombo("##DashboardPicker", preview.c_str())) {
					if (ImGui::Selectable("(self) ImGuiVRHelper", activePicker == 0)) {
						Dashboard::SetActiveClient(0);
					}
					for (const auto& c : clients) {
						if (!c.dashboard_eligible)
							continue;
						if (c.client_id == selfId)
							continue;  // already shown as "(self)" entry
						std::string label = c.name;
						if (!c.version.empty())
							label += " " + c.version;
						// Annotate clients that haven't acked the
						// focus-render contract so the user knows the
						// picker won't auto-show their menu.
						if (!(c.flags & API::kClientFlag_RendersOnFocus)) {
							label += "  (manual trigger)";
						}
						if (ImGui::Selectable(label.c_str(), c.client_id == activePicker)) {
							Dashboard::SetActiveClient(c.client_id);
						}
					}
					ImGui::EndCombo();
				}

				// Banner when a non-honoring client is selected. The
				// dashboard surface stays on the helper's settings
				// panel (Dashboard::ResolveActiveTexture falls back to
				// self-client) so the user can pick something else
				// without leaving the dashboard.
				if (activePicker != 0) {
					for (const auto& c : clients) {
						if (c.client_id != activePicker)
							continue;
						if (!(c.flags & API::kClientFlag_RendersOnFocus)) {
							ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
								"Trigger %s manually to display its menu.",
								c.name.c_str());
							ImGui::TextDisabled(
								"This client doesn't auto-render on focus; "
								"use its own hotkey / activation.");
						}
						break;
					}
				}

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
					kCol_Dashboard = 5,
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

				if (ImGui::BeginTable("##ClientsTable", 6, kTableFlags, outerSize)) {
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
					ImGui::TableSetupColumn("Dashboard",
						ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending,
						100.0f, kCol_Dashboard);
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
									case kCol_Dashboard:
										{
											// Rank: active (2) > eligible (1) > none (0).
											// Descending puts "shown in dashboard right now" at
											// the top, then "available in the picker," then
											// non-dashboard clients.
											auto dashRank = [](const HelperImpl::ClientSnapshot& c) {
												if (c.dashboard_active)
													return 2;
												if (c.dashboard_eligible)
													return 1;
												return 0;
											};
											const int ra = dashRank(a), rb = dashRank(b);
											cmp = (ra < rb) ? -1 : (ra > rb) ? 1 :
										                                       0;
										}
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
						ImGui::TableNextColumn();
						if (c.dashboard_active) {
							ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "ACTIVE");
						} else if (c.dashboard_eligible) {
							ImGui::TextDisabled("eligible");
						} else {
							ImGui::TextDisabled("-");
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
				ImGui::TextDisabled("Dashboard:   ACTIVE   = currently shown on SteamVR dashboard plane");
				ImGui::TextDisabled("             eligible = listed in the dashboard picker below");
				ImGui::TextDisabled("             -        = not opted into kClientFlag_Dashboard");
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
				ImGui::TextDisabled("HUD-mode geometry (all kClientFlag_HUDMode clients)");
				ImGui::SliderFloat("HUD depth (m)", &s.hudDepth, 0.5f, 3.0f, "%.2f");
				ImGui::SliderFloat("HUD FOV (deg)", &s.hudFOV, 30.0f, 100.0f, "%.0f");
				ImGui::TextDisabled("    Closer + narrower = 'glass layer / monitor' feel.");
				ImGui::TextDisabled("    Farther + wider = 'billboard in space' feel.");
				ImGui::TextDisabled("    If panel edges are clipped by the lens, lower FOV.");
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

	bool IsVisible() { return g_visible || g_forceVisible; }

	void SetForceVisible(bool forced) { g_forceVisible = forced; }

	ImGuiContext* GetContext() { return g_ctx; }

	bool Render(float dt)
	{
		if (!g_ctx)
			return false;
		// Render whenever the settings window is up (toggled or
		// dashboard-forced) or the combo recording modal is active.
		// The modal can fire from any client at any time.
		if (!g_visible && !g_forceVisible && !ComboRecording::IsActive())
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
		const bool isDragging = vrState.dragState.dragging;
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
		// Win32 input backend's NewFrame captures cursor pos + key state
		// from Windows and feeds it into this context's IO. Skipped if
		// the WndProc hook didn't install (e.g. swapchain HWND wasn't
		// resolvable) — controllers still drive ImGui via the thumbstick
		// + button paths above, just without desktop input.
		if (g_hwnd) {
			ImGui_ImplWin32_NewFrame();
		}
		ImGui::NewFrame();
		if (g_visible || g_forceVisible) {
			RenderWindow();
		}
		ComboRecording::RenderModal();
		ImGui::Render();
		return true;
	}
}
