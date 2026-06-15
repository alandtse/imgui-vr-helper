// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include "pch.h"

#include "Dashboard.h"

#include "HelperImpl.h"
#include "Overlay.h"
#include "SettingsUI.h"
#include "internal/VRUtils.h"

#include <RE/B/BSOpenVR.h>
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
		bool g_loggedRuntimeUnavailable = false;

		// Rate-limit CreateDashboardOverlay failure logging so a runtime
		// that genuinely can't host dashboard overlays (e.g. OpenComposite)
		// doesn't spam every Present. We keep RETRYING creation each frame
		// (the call is cheap) rather than latching a permanent failure —
		// the first Tick can fire during renderer init, before SteamVR's
		// overlay system is ready, and a one-time early failure must not
		// disable the dashboard for the whole session.
		constexpr int kCreateRetryLogInterval = 600;
		int g_createFailLogCountdown = 0;

		// Diagnostics latches: log the first texture-submit error and the
		// first successful submit exactly once so CommunityShaders-style
		// logs distinguish "overlay created" from "actually showing pixels".
		bool g_loggedSubmitError = false;
		bool g_loggedSubmitOk = false;

		// One-time probe: when CreateDashboardOverlay fails, try a REGULAR
		// CreateOverlay once to disambiguate the failure. If the regular
		// overlay succeeds but the dashboard one doesn't, the gate is
		// dashboard-specific (SteamVR reserves dashboard overlays for
		// VRApplication_Overlay apps; Skyrim is VRApplication_Scene) and an
		// in-process dashboard tile isn't achievable. If both fail, it's a
		// broader denial.
		bool g_probedRegularOverlay = false;

		// Active dashboard client. 0 means "use the helper's self-client"
		// (resolved at Tick time so the self-client doesn't have to exist
		// at module-init).
		uint32_t g_activeClient = 0;

		// Cached state.
		bool g_dashboardVisible = false;
		// Whether the client whose panel the dashboard is currently
		// mirroring resolved to the helper's own self-client (either the
		// default selection or a fallback for a non-honoring client).
		// Lets HelperImpl force-render the settings UI so the rail entry
		// shows a live menu. Updated each Tick.
		bool g_resolvedIsSelf = false;

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

		// Proxied (game) interface — valid for creation, positioning,
		// event-pump, IsDashboardVisible, DestroyOverlay.
		vr::IVROverlay* GetOverlayInterface()
		{
			Util::OpenVRContext ctx;
			return ctx.IsValid() ? ctx.overlay : nullptr;
		}

		// Clean (unproxied) interface obtained directly from openvr_api.
		// REQUIRED for texture submission: the game's proxied IVROverlay
		// lacks SetOverlayTexture permission for a custom DLL and returns
		// VROverlayError_PermissionDenied (see RE/B/BSOpenVR.h). Overlay
		// handles are runtime-global, so the handle created on the proxied
		// interface is valid here. May be nullptr on non-OpenVR runtimes.
		vr::IVROverlay* GetSubmitInterface()
		{
			return RE::BSOpenVR::GetCleanIVROverlay();
		}

		/// Allocate the shared dashboard overlay if possible. Idempotent
		/// once created; retries creation every Tick while g_overlay is 0
		/// (cheap), rate-limiting the failure log so an unsupported runtime
		/// doesn't spam. Deliberately does NOT permanently latch a failure —
		/// the first Tick can fire before SteamVR's overlay system is ready.
		bool EnsureOverlay()
		{
			if (g_overlay != 0)
				return true;

			// Create on the CLEAN (own-app) interface, not the game's proxied
			// one. The proxied IVROverlay returned VROverlayError_InvalidHandle
			// for CreateDashboardOverlay — the overlay must be created and
			// submitted on the same unproxied interface.
			vr::IVROverlay* iface = GetSubmitInterface();
			if (!iface) {
				if (g_createFailLogCountdown <= 0) {
					g_createFailLogCountdown = kCreateRetryLogInterval;
					logs::warn("Dashboard: clean IVROverlay unavailable; retrying");
				} else {
					--g_createFailLogCountdown;
				}
				return false;
			}

			auto err = iface->CreateDashboardOverlay(
				kOverlayKey, kOverlayName, &g_overlay, &g_thumbnail);
			if (err != vr::VROverlayError_None) {
				g_overlay = 0;  // CreateDashboardOverlay may have partially written
				if (g_createFailLogCountdown <= 0) {
					g_createFailLogCountdown = kCreateRetryLogInterval;
					logs::warn("Dashboard: CreateDashboardOverlay (clean) failed: {} (retrying)",
						iface->GetOverlayErrorNameFromEnum(err));

					// One-shot: probe a regular (non-dashboard) overlay so the
					// next failure log tells us whether the gate is
					// dashboard-specific (app-type) or a broader denial.
					if (!g_probedRegularOverlay) {
						g_probedRegularOverlay = true;
						vr::VROverlayHandle_t probe = 0;
						auto perr = iface->CreateOverlay("imgui-vr-helper.probe", "ImGuiVRHelper probe", &probe);
						logs::warn("Dashboard: probe CreateOverlay(regular, clean) -> {}",
							iface->GetOverlayErrorNameFromEnum(perr));
						if (perr == vr::VROverlayError_None && probe != 0)
							iface->DestroyOverlay(probe);
					}
				} else {
					--g_createFailLogCountdown;
				}
				return false;
			}

			iface->SetOverlayWidthInMeters(g_overlay, 2.5f);
			iface->SetOverlayInputMethod(g_overlay, vr::VROverlayInputMethod_Mouse);

			vr::HmdVector2_t mouseScale{
				static_cast<float>(Overlay::Config::kOverlayWidth),
				static_cast<float>(Overlay::Config::kOverlayHeight)
			};
			iface->SetOverlayMouseScale(g_overlay, &mouseScale);

			logs::info("Dashboard: registered SteamVR dashboard overlay key='{}' (handle={}) on clean interface",
				kOverlayKey, g_overlay);
			return true;
		}

		/// Apply any pending thumbnail upload. Cheap when no change pending.
		void ApplyPendingThumbnail(vr::IVROverlay* iface)
		{
			if (!g_thumbnailDirty || g_thumbnail == 0)
				return;

			// Texture-data calls must go through the clean interface (same
			// permission constraint as SetOverlayTexture). Defer until it's
			// available rather than silently failing on the proxied one.
			vr::IVROverlay* submit = GetSubmitInterface();
			if (!submit)
				return;
			g_thumbnailDirty = false;

			if (g_pendingThumbnailPath.empty()) {
				submit->ClearOverlayTexture(g_thumbnail);
				logs::info("Dashboard: thumbnail cleared (placeholder)");
				return;
			}

			const auto err = submit->SetOverlayFromFile(
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
		/// Returns a com_ptr COPY (AddRef'd while m_mutex is held) so the
		/// texture survives a concurrent UnregisterClient between this
		/// return and the SetOverlayTexture call in Tick — returning a raw
		/// pointer would be a use-after-free if the client unregisters in
		/// that window. SteamVR's SetOverlayTexture copies the texture, so
		/// the caller only needs to keep it alive across that one call.
		winrt::com_ptr<ID3D11Texture2D> ResolveActiveTexture(HelperImpl& impl, uint32_t& out_client_id)
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
			return it->second.texture;  // com_ptr copy AddRefs under the lock
		}

		/// Drain VREvent_* from the shared dashboard overlay. Mouse / scroll
		/// must be injected into the ImGui context whose panel the dashboard
		/// is mirroring — for the self-client that's SettingsUI's context;
		/// `targetIsSelf` says whether that's the case this frame. External
		/// clients own their ImGui context in their own DLL, which the
		/// helper can't SetCurrentContext to, so their dashboard mouse input
		/// isn't routed (they still get controller/wand input via the
		/// in-scene path). Injecting into "whatever context is current"
		/// would land clicks on the wrong context, so we skip when we can't
		/// target the right one. Runs on the Present thread (same thread
		/// that owns these contexts), so SetCurrentContext is safe here.
		void PumpEvents(vr::IVROverlay* iface, bool targetIsSelf)
		{
			ImGuiContext* const target = targetIsSelf ? SettingsUI::GetContext() : nullptr;
			ImGuiContext* prevCtx = nullptr;
			bool switched = false;
			// Switch to the target context lazily, only once we actually
			// have a mouse/scroll event to deliver.
			auto ensureCtx = [&]() -> bool {
				if (!target)
					return false;
				if (!switched) {
					prevCtx = ImGui::GetCurrentContext();
					ImGui::SetCurrentContext(target);
					switched = true;
				}
				return true;
			};

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
					if (ensureCtx())
						ImGui::GetIO().AddMousePosEvent(evt.data.mouse.x, evt.data.mouse.y);
					break;

				case vr::VREvent_MouseButtonDown:
				case vr::VREvent_MouseButtonUp:
					if (ensureCtx()) {
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
						ImGui::GetIO().AddMouseButtonEvent(btn, down);
					}
					break;

				case vr::VREvent_Scroll:
					if (ensureCtx())
						ImGui::GetIO().AddMouseWheelEvent(
							evt.data.scroll.xdelta, evt.data.scroll.ydelta);
					break;

				default:
					break;
				}
			}

			if (switched)
				ImGui::SetCurrentContext(prevCtx);
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

		if (!EnsureOverlay())
			return;

		ApplyPendingThumbnail(iface);
		g_dashboardVisible = iface->IsDashboardVisible();

		auto& impl = HelperImpl::GetSingleton();

		uint32_t resolved_client = 0;
		// Hold the com_ptr in a stack local so the texture can't be freed
		// by a concurrent UnregisterClient before SetOverlayTexture runs.
		winrt::com_ptr<ID3D11Texture2D> tex = ResolveActiveTexture(impl, resolved_client);
		const uint32_t selfId = impl.GetSelfClientId();
		g_resolvedIsSelf = (resolved_client != 0 && resolved_client == selfId);
		if (tex) {
			// Texture submission MUST use the clean interface — the proxied
			// one returns VROverlayError_PermissionDenied from a custom DLL
			// (RE/B/BSOpenVR.h). This is the call that makes the dashboard
			// panel actually show pixels.
			if (vr::IVROverlay* submit = GetSubmitInterface()) {
				vr::Texture_t t{};
				t.handle = tex.get();
				t.eType = vr::TextureType_DirectX;
				t.eColorSpace = vr::ColorSpace_Auto;
				const auto err = submit->SetOverlayTexture(g_overlay, &t);
				if (err != vr::VROverlayError_None) {
					if (!g_loggedSubmitError) {
						g_loggedSubmitError = true;
						logs::warn(
							"Dashboard: SetOverlayTexture failed: {} "
							"(dashboard entry will appear blank)",
							iface->GetOverlayErrorNameFromEnum(err));
					}
				} else if (!g_loggedSubmitOk) {
					g_loggedSubmitOk = true;
					logs::info("Dashboard: panel populated (texture submitted via clean interface)");
				}
			} else if (!g_loggedSubmitError) {
				g_loggedSubmitError = true;
				logs::warn("Dashboard: clean IVROverlay unavailable; cannot submit panel texture");
			}
		}

		// Poll overlay events on the clean interface that owns the overlay
		// (the same one it was created and texture-submitted on). Falls back
		// to the proxied iface if the clean one is momentarily unavailable.
		if (vr::IVROverlay* eventIface = GetSubmitInterface())
			PumpEvents(eventIface, g_resolvedIsSelf);
		else
			PumpEvents(iface, g_resolvedIsSelf);
	}

	void Shutdown()
	{
		// Destroy on the clean interface that created/owns the overlay.
		if (vr::IVROverlay* iface = GetSubmitInterface(); iface && g_overlay != 0) {
			iface->DestroyOverlay(g_overlay);
		}
		g_overlay = 0;
		g_thumbnail = 0;
		g_loggedSubmitError = false;
		g_loggedSubmitOk = false;
		g_probedRegularOverlay = false;
	}

	bool IsDashboardVisible()
	{
		return g_dashboardVisible;
	}

	bool IsShowingSelf()
	{
		return g_resolvedIsSelf;
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
