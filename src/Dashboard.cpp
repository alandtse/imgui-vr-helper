// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include "pch.h"

#include "Dashboard.h"

#include "Globals.h"
#include "HelperImpl.h"
#include "Overlay.h"
#include "internal/VRUtils.h"

#include <openvr.h>

namespace ImGuiVRHelper
{
	// Friend type with access to HelperImpl's private members. Separates the
	// "Dashboard needs raw map + mutex" coupling from the rest of the
	// HelperImpl API so the rest of the helper doesn't have to grow public
	// accessors purely to satisfy the dashboard subsystem.
	struct DashboardFriend
	{
		static std::mutex& Mutex(HelperImpl& impl) { return impl.m_mutex; }
		static std::unordered_map<uint32_t, ClientRecord>& Clients(HelperImpl& impl)
		{
			return impl.m_clients;
		}
		static uint32_t& FocusedClient(HelperImpl& impl) { return impl.m_focused_client; }
	};
}

namespace ImGuiVRHelper::Dashboard
{
	namespace
	{
		// Cached "is the SteamVR dashboard up right now" flag. Updated
		// once per Tick — IVROverlay::IsDashboardVisible is cheap but
		// we don't need sub-frame freshness anywhere.
		bool g_dashboardVisible = false;

		// Logged once on detection so a user troubleshooting why their
		// dashboard entry isn't showing has something to grep in
		// CommunityShaders.log without having to enable trace logging.
		bool g_loggedRuntimeUnavailable = false;

		/// Returns the IVROverlay interface or nullptr if the runtime
		/// hasn't initialised it. Call every frame — OpenComposite-based
		/// runtimes can return nullptr indefinitely without ever throwing,
		/// so we silently degrade by skipping the dashboard path.
		vr::IVROverlay* GetOverlayInterface()
		{
			Util::OpenVRContext ctx;
			return ctx.IsValid() ? ctx.overlay : nullptr;
		}

		/// Build a stable overlay key for SteamVR's position-memory
		/// system. SteamVR persists per-key dashboard layout (position,
		/// size, gain), so reusing the same key across sessions means
		/// the user's dashboard tweaks stick across launches.
		std::string MakeOverlayKey(const ClientRecord& rec)
		{
			// std::hash<string> isn't required to be stable across STL
			// releases but is in practice on MSVC. If that ever changes,
			// swap in FNV-1a; the cost is one-time loss of SteamVR-
			// remembered dashboard layout for existing users.
			const auto h = std::hash<std::string>{}(rec.name);
			return std::format("imgui-vr-helper.client.{:016x}", h);
		}

		/// Friendly name shown next to the thumbnail in SteamVR's
		/// dashboard rail. Falls back to client name if no version is set.
		std::string MakeOverlayName(const ClientRecord& rec)
		{
			if (rec.version.empty()) {
				return rec.name;
			}
			return std::format("{} {}", rec.name, rec.version);
		}

		/// Apply the thumbnail asset to a dashboard overlay's thumbnail
		/// surface. Path is resolved relative to the game root
		/// (Data/SKSE/Plugins/...) when not absolute. Lazy — called the
		/// first time the dashboard surface is shown to avoid disk I/O
		/// during registration.
		void LoadThumbnailIfNeeded(vr::IVROverlay* iface, ClientRecord& rec)
		{
			if (rec.dashboard_thumbnail == 0 || rec.dashboard_thumbnail_path.empty())
				return;

			// SetOverlayFromFile reads the file synchronously and uploads
			// to SteamVR. Errors here aren't fatal — SteamVR shows its
			// generic placeholder icon instead.
			const auto err = iface->SetOverlayFromFile(
				rec.dashboard_thumbnail,
				rec.dashboard_thumbnail_path.c_str());
			if (err != vr::VROverlayError_None) {
				logs::warn("Dashboard: SetOverlayFromFile('{}') for client '{}' failed: {}",
					rec.dashboard_thumbnail_path, rec.name,
					iface->GetOverlayErrorNameFromEnum(err));
			} else {
				logs::info("Dashboard: thumbnail loaded for client '{}' from '{}'",
					rec.name, rec.dashboard_thumbnail_path);
			}
			// One-shot: clear the path so we don't re-upload on every
			// activation. SetDashboardThumbnail re-fills the path if the
			// caller wants to swap icons.
			rec.dashboard_thumbnail_path.clear();
		}

		/// Mirror a client's panel texture onto its dashboard surface.
		/// SteamVR copies on SetOverlayTexture, so the source texture
		/// can be re-used for the in-scene path the same frame.
		void MirrorTexture(vr::IVROverlay* iface, ClientRecord& rec)
		{
			if (rec.dashboard_overlay == 0 || !rec.texture)
				return;

			vr::Texture_t tex{};
			tex.handle = rec.texture.get();
			tex.eType = vr::TextureType_DirectX;
			tex.eColorSpace = vr::ColorSpace_Auto;
			iface->SetOverlayTexture(rec.dashboard_overlay, &tex);

			// Mouse-input scale: tell SteamVR our panel coordinates run
			// 0..width × 0..height so VREvent_MouseMove arrives in
			// pixels we can hand to ImGui directly without rescaling.
			vr::HmdVector2_t mouseScale{
				static_cast<float>(Overlay::Config::kOverlayWidth),
				static_cast<float>(Overlay::Config::kOverlayHeight)
			};
			iface->SetOverlayMouseScale(rec.dashboard_overlay, &mouseScale);
		}

		/// Drain VREvent_* from a single dashboard overlay. Returns
		/// (open_changed, now_open) so the caller can apply focus
		/// changes without holding the helper lock during RequestFocus.
		///
		/// Mouse / scroll events translate to ImGui input state inline —
		/// safe because ImGui's IO doesn't take the helper's mutex.
		struct PumpResult
		{
			bool open_changed = false;
			bool now_open = false;
		};
		PumpResult PumpEvents(vr::IVROverlay* iface, ClientRecord& rec)
		{
			PumpResult result{};
			const bool was_open = rec.dashboard_open;

			vr::VREvent_t evt{};
			while (iface->PollNextOverlayEvent(rec.dashboard_overlay, &evt, sizeof(evt))) {
				switch (evt.eventType) {
				case vr::VREvent_OverlayShown:
					rec.dashboard_open = true;
					LoadThumbnailIfNeeded(iface, rec);
					logs::info("Dashboard: overlay shown for client '{}'", rec.name);
					break;

				case vr::VREvent_OverlayHidden:
					rec.dashboard_open = false;
					logs::info("Dashboard: overlay hidden for client '{}'", rec.name);
					break;

				case vr::VREvent_MouseMove:
					{
						auto& io = ImGui::GetIO();
						io.AddMousePosEvent(evt.data.mouse.x, evt.data.mouse.y);
					}
					break;

				case vr::VREvent_MouseButtonDown:
				case vr::VREvent_MouseButtonUp:
					{
						auto& io = ImGui::GetIO();
						const bool down = evt.eventType == vr::VREvent_MouseButtonDown;
						// SteamVR uses VRMouseButton_Left/Right/Middle ==
						// 1/2/4 (bit-flag). ImGui wants 0/1/2.
						int btn = 0;
						switch (evt.data.mouse.button) {
						case vr::VRMouseButton_Left:
							btn = 0;
							break;
						case vr::VRMouseButton_Right:
							btn = 1;
							break;
						case vr::VRMouseButton_Middle:
							btn = 2;
							break;
						default:
							btn = 0;
							break;
						}
						io.AddMouseButtonEvent(btn, down);
					}
					break;

				case vr::VREvent_Scroll:
					{
						auto& io = ImGui::GetIO();
						io.AddMouseWheelEvent(
							evt.data.scroll.xdelta, evt.data.scroll.ydelta);
					}
					break;

				default:
					// Many event types we don't care about
					// (VREvent_FocusEnter/Leave, VREvent_LockMousePos,
					// keyboard input from SteamVR's virtual keyboard, etc.).
					// Silent discard — phase 2 hooks more if we need them.
					break;
				}
			}

			if (rec.dashboard_open != was_open) {
				result.open_changed = true;
				result.now_open = rec.dashboard_open;
			}
			return result;
		}
	}

	bool EnsureClientLocked(uint32_t client_id, ClientRecord& rec)
	{
		if (rec.dashboard_overlay != 0)
			return true;
		if (rec.dashboard_init_failed)
			return false;
		if (!(rec.flags & ImGuiVRHelperPluginAPI::kClientFlag_Dashboard))
			return false;

		auto* iface = GetOverlayInterface();
		if (!iface) {
			if (!g_loggedRuntimeUnavailable) {
				g_loggedRuntimeUnavailable = true;
				logs::info(
					"Dashboard: IVROverlay interface unavailable; "
					"kClientFlag_Dashboard clients will fall back to "
					"in-scene-only rendering. (OpenComposite-based "
					"runtimes typically don't implement dashboard "
					"overlays.)");
			}
			return false;
		}

		const auto key = MakeOverlayKey(rec);
		const auto name = MakeOverlayName(rec);

		vr::VROverlayHandle_t overlay = 0;
		vr::VROverlayHandle_t thumbnail = 0;
		auto err = iface->CreateDashboardOverlay(
			key.c_str(), name.c_str(), &overlay, &thumbnail);
		if (err != vr::VROverlayError_None) {
			rec.dashboard_init_failed = true;
			logs::warn("Dashboard: CreateDashboardOverlay('{}') for client '{}' failed: {}",
				key, rec.name, iface->GetOverlayErrorNameFromEnum(err));
			return false;
		}

		// Default geometry: 2.5m wide; the panel aspect controls height.
		// SteamVR persists user tweaks via the overlay key, so this only
		// matters on first run.
		iface->SetOverlayWidthInMeters(overlay, 2.5f);
		iface->SetOverlayInputMethod(overlay, vr::VROverlayInputMethod_Mouse);

		rec.dashboard_overlay = overlay;
		rec.dashboard_thumbnail = thumbnail;
		rec.dashboard_open = false;

		logs::info("Dashboard: created overlay key='{}' for client '{}' (id={})",
			key, rec.name, client_id);
		return true;
	}

	void ReleaseClientLocked(ClientRecord& rec)
	{
		if (rec.dashboard_overlay == 0)
			return;
		auto* iface = GetOverlayInterface();
		if (iface) {
			iface->DestroyOverlay(rec.dashboard_overlay);
			// Thumbnail handle is destroyed implicitly when the parent
			// dashboard overlay goes away — explicit DestroyOverlay on
			// the thumbnail produces VROverlayError_InvalidHandle.
		}
		rec.dashboard_overlay = 0;
		rec.dashboard_thumbnail = 0;
		rec.dashboard_open = false;
	}

	void Tick()
	{
		auto* iface = GetOverlayInterface();
		if (!iface) {
			g_dashboardVisible = false;
			return;
		}

		g_dashboardVisible = iface->IsDashboardVisible();

		auto& impl = HelperImpl::GetSingleton();

		// Drive each dashboard client under the helper lock for atomicity
		// against UnregisterClient. RequestFocus itself takes the same
		// lock (m_mutex is std::mutex, not recursive), so we record any
		// focus flip during iteration and apply it after the lock drops.
		uint32_t focus_target = 0;

		{
			std::scoped_lock lk{ DashboardFriend::Mutex(impl) };
			auto& clients = DashboardFriend::Clients(impl);

			for (auto& [id, rec] : clients) {
				if (!(rec.flags & ImGuiVRHelperPluginAPI::kClientFlag_Dashboard))
					continue;

				if (rec.dashboard_overlay == 0 && !EnsureClientLocked(id, rec)) {
					continue;
				}

				MirrorTexture(iface, rec);

				const auto pump = PumpEvents(iface, rec);
				if (pump.open_changed && pump.now_open) {
					// Newly-shown dashboard surface takes focus: same
					// single-focus model as the in-scene panel. If
					// multiple dashboard clients open the same frame
					// (rare — the user can only be hovering one),
					// last-wins in iteration order.
					focus_target = id;
				}
			}
		}  // helper lock released here

		if (focus_target != 0) {
			impl.RequestFocus(focus_target);
		}
	}

	bool IsDashboardVisible()
	{
		return g_dashboardVisible;
	}
}
