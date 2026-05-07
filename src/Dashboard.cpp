// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include "pch.h"

#include "Dashboard.h"

#include "HelperImpl.h"
#include "Overlay.h"
#include "internal/VRUtils.h"

#include <openvr.h>

namespace ImGuiVRHelper
{
	struct DashboardFriend
	{
		static std::mutex& Mutex(HelperImpl& impl) { return impl.m_mutex; }
		static std::unordered_map<uint32_t, ClientRecord>& Clients(HelperImpl& impl)
		{
			return impl.m_clients;
		}
	};
}

namespace ImGuiVRHelper::Dashboard
{
	namespace
	{
		// One overlay + thumbnail for the whole helper. Created lazily on
		// first Tick when IVROverlay is available.
		vr::VROverlayHandle_t g_overlay = 0;
		vr::VROverlayHandle_t g_thumbnail = 0;
		bool g_initFailed = false;
		bool g_loggedRuntimeUnavailable = false;

		// Active dashboard client. 0 means "use the helper's self-client"
		// (resolved at Tick time so the self-client doesn't have to exist
		// at module-init).
		uint32_t g_activeClient = 0;

		// Cached state.
		bool g_dashboardVisible = false;

		// Thumbnail update queue. Loaded on the next Tick on the render
		// thread to keep file I/O off whichever thread called
		// SetThumbnail. Empty string means "use SteamVR placeholder".
		std::string g_pendingThumbnailPath;
		bool g_thumbnailDirty = false;

		// Overlay key — fixed per helper install. SteamVR persists user
		// dashboard layout (position, size, gain) keyed on this string,
		// so renaming it loses the user's tweaks.
		constexpr const char* kOverlayKey = "imgui-vr-helper.dashboard";
		constexpr const char* kOverlayName = "ImGuiVRHelper";

		vr::IVROverlay* GetOverlayInterface()
		{
			Util::OpenVRContext ctx;
			return ctx.IsValid() ? ctx.overlay : nullptr;
		}

		/// Allocate the shared dashboard overlay if possible. Idempotent;
		/// latches `g_initFailed` on hard errors so we don't retry every
		/// Tick on runtimes that don't support dashboard overlays.
		bool EnsureOverlay(vr::IVROverlay* iface)
		{
			if (g_overlay != 0)
				return true;
			if (g_initFailed)
				return false;

			auto err = iface->CreateDashboardOverlay(
				kOverlayKey, kOverlayName, &g_overlay, &g_thumbnail);
			if (err != vr::VROverlayError_None) {
				g_initFailed = true;
				logs::warn("Dashboard: CreateDashboardOverlay failed: {}",
					iface->GetOverlayErrorNameFromEnum(err));
				return false;
			}

			iface->SetOverlayWidthInMeters(g_overlay, 2.5f);
			iface->SetOverlayInputMethod(g_overlay, vr::VROverlayInputMethod_Mouse);

			vr::HmdVector2_t mouseScale{
				static_cast<float>(Overlay::Config::kOverlayWidth),
				static_cast<float>(Overlay::Config::kOverlayHeight)
			};
			iface->SetOverlayMouseScale(g_overlay, &mouseScale);

			logs::info("Dashboard: created shared overlay key='{}'", kOverlayKey);
			return true;
		}

		/// Apply any pending thumbnail upload. Cheap when no change pending.
		void ApplyPendingThumbnail(vr::IVROverlay* iface)
		{
			if (!g_thumbnailDirty || g_thumbnail == 0)
				return;
			g_thumbnailDirty = false;

			if (g_pendingThumbnailPath.empty()) {
				iface->ClearOverlayTexture(g_thumbnail);
				logs::info("Dashboard: thumbnail cleared (placeholder)");
				return;
			}

			const auto err = iface->SetOverlayFromFile(
				g_thumbnail, g_pendingThumbnailPath.c_str());
			if (err == vr::VROverlayError_None) {
				logs::info("Dashboard: thumbnail loaded from '{}'", g_pendingThumbnailPath);
			} else {
				logs::warn("Dashboard: SetOverlayFromFile('{}') failed: {}",
					g_pendingThumbnailPath, iface->GetOverlayErrorNameFromEnum(err));
			}
		}

		/// Resolve which client's panel texture to mirror onto the
		/// dashboard surface. Returns the self-client texture in three
		/// cases:
		///   1. No active picker selection (g_activeClient == 0).
		///   2. Active client was unregistered (not found in m_clients).
		///   3. Active client lacks kClientFlag_RendersOnFocus — they
		///      don't honor the focus-render contract, so their RTV
		///      may be stale or empty. Mirroring the helper's own
		///      settings panel keeps the user able to navigate (the
		///      picker UI is right there); SettingsUI shows an inline
		///      banner explaining the manual-trigger requirement.
		///
		/// Caller (Tick) takes the texture handle out of the locked
		/// scope; SteamVR's SetOverlayTexture copies before returning
		/// so the texture lifetime past this call doesn't matter.
		ID3D11Texture2D* ResolveActiveTexture(HelperImpl& impl, uint32_t& out_client_id)
		{
			std::scoped_lock lk{ DashboardFriend::Mutex(impl) };
			auto& clients = DashboardFriend::Clients(impl);

			uint32_t target = g_activeClient;
			if (target == 0 || !clients.contains(target)) {
				target = impl.GetSelfClientId();
			} else {
				const auto& rec = clients.at(target);
				if (!(rec.flags & ImGuiVRHelperPluginAPI::kClientFlag_RendersOnFocus)) {
					// Non-honoring client picked: keep showing the
					// helper's settings panel so the user can pick
					// something else or dismiss. SettingsUI checks
					// GetActiveClient against the per-snapshot flag
					// to know whether to show the manual-trigger banner.
					target = impl.GetSelfClientId();
				}
			}

			out_client_id = target;
			if (target == 0)
				return nullptr;

			auto it = clients.find(target);
			if (it == clients.end())
				return nullptr;
			return it->second.texture.get();
		}

		/// Drain VREvent_* from the shared dashboard overlay. Mouse /
		/// scroll feed straight into ImGui IO; show/hide flips
		/// g_dashboardVisible.
		void PumpEvents(vr::IVROverlay* iface)
		{
			vr::VREvent_t evt{};
			while (iface->PollNextOverlayEvent(g_overlay, &evt, sizeof(evt))) {
				switch (evt.eventType) {
				case vr::VREvent_OverlayShown:
					logs::info("Dashboard: overlay shown");
					break;
				case vr::VREvent_OverlayHidden:
					logs::info("Dashboard: overlay hidden");
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
					break;
				}
			}
		}
	}

	void Tick()
	{
		auto* iface = GetOverlayInterface();
		if (!iface) {
			if (!g_loggedRuntimeUnavailable) {
				g_loggedRuntimeUnavailable = true;
				logs::info(
					"Dashboard: IVROverlay interface unavailable; "
					"SteamVR dashboard integration disabled. "
					"(OpenComposite-based runtimes typically don't "
					"implement dashboard overlays.)");
			}
			g_dashboardVisible = false;
			return;
		}

		if (!EnsureOverlay(iface))
			return;

		ApplyPendingThumbnail(iface);
		g_dashboardVisible = iface->IsDashboardVisible();

		auto& impl = HelperImpl::GetSingleton();

		uint32_t resolved_client = 0;
		ID3D11Texture2D* tex = ResolveActiveTexture(impl, resolved_client);
		if (tex) {
			vr::Texture_t t{};
			t.handle = tex;
			t.eType = vr::TextureType_DirectX;
			t.eColorSpace = vr::ColorSpace_Auto;
			iface->SetOverlayTexture(g_overlay, &t);
		}

		PumpEvents(iface);
	}

	void Shutdown()
	{
		auto* iface = GetOverlayInterface();
		if (iface && g_overlay != 0) {
			iface->DestroyOverlay(g_overlay);
		}
		g_overlay = 0;
		g_thumbnail = 0;
		g_initFailed = false;
	}

	bool IsDashboardVisible()
	{
		return g_dashboardVisible;
	}

	void SetActiveClient(uint32_t client_id)
	{
		auto& impl = HelperImpl::GetSingleton();
		// Validate under the lock — the picker is racing client
		// register/unregister.
		{
			std::scoped_lock lk{ DashboardFriend::Mutex(impl) };
			auto& clients = DashboardFriend::Clients(impl);
			if (client_id != 0 && !clients.contains(client_id)) {
				return;
			}
			if (client_id != 0) {
				const auto& rec = clients.at(client_id);
				if (!(rec.flags & ImGuiVRHelperPluginAPI::kClientFlag_Dashboard)) {
					logs::warn(
						"Dashboard::SetActiveClient({}): client lacks "
						"kClientFlag_Dashboard; ignored",
						client_id);
					return;
				}
			}
			g_activeClient = client_id;
		}

		const uint32_t focus_target = (client_id != 0) ? client_id : impl.GetSelfClientId();
		if (focus_target != 0) {
			impl.RequestFocus(focus_target);
		}
		logs::info("Dashboard: active client = {} ({})",
			focus_target,
			client_id == 0 ? "self" : "external");
	}

	uint32_t GetActiveClient()
	{
		return g_activeClient;
	}

	void ClearActiveClientIfMatches(uint32_t client_id)
	{
		// Caller (HelperImpl::UnregisterClient) already holds m_mutex.
		// We don't take it here — std::mutex isn't recursive — and we
		// don't reach back into HelperImpl::RequestFocus either; the
		// next Tick will resolve to the self-client and the in-scene
		// reconciler picks up focus changes.
		if (g_activeClient == client_id) {
			g_activeClient = 0;
		}
	}

	void SetThumbnail(const char* image_path)
	{
		g_pendingThumbnailPath = image_path ? image_path : "";
		g_thumbnailDirty = true;
	}
}
