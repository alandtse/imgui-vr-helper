// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Stub implementations. Each method logs and returns a default value. Real
// behavior arrives in subsequent PRs (overlay submission, controller
// polling, combo matching, drag, wand pointing, in-scene fallback).

#include "pch.h"

#include "ComboRecording.h"
#include "Globals.h"
#include "HelperImpl.h"
#include "Input.h"
#include "Overlay.h"
#include "OverlayDrag.h"
#include "OverlayManager.h"
#include "OverlayTinter.h"
#include "SettingsUI.h"
#include "WandPointing.h"
#include "internal/VRUtils.h"

#include <RE/B/BSOpenVRControllerDevice.h>

#include <nlohmann/json.hpp>

namespace
{
	/// Returns true iff every key in the combo is currently held on the
	/// expected device. Mirrors SCS's CheckCombo lambda: reads the live
	/// per-controller state from Overlay::State, indexed by RE button key
	/// codes. Keyboard/mouse/gamepad device types are unsupported here
	/// (they need a Win32 input source we don't have yet).
	bool MatchCombo(const std::vector<ImGuiVRHelperPluginAPI::InputCombo>& keys)
	{
		if (keys.empty())
			return false;

		namespace API = ImGuiVRHelperPluginAPI;
		auto& state = ImGuiVRHelper::Overlay::State::GetSingleton();

		for (const auto& k : keys) {
			const uint32_t reKey = k.GetKey();
			bool pressed = false;
			switch (k.GetDevice()) {
			case API::InputDeviceType::Both:
				pressed = state.primaryControllerState[reKey].isPressed &&
				          state.secondaryControllerState[reKey].isPressed;
				break;
			case API::InputDeviceType::Primary:
				pressed = state.primaryControllerState[reKey].isPressed;
				break;
			case API::InputDeviceType::Secondary:
				pressed = state.secondaryControllerState[reKey].isPressed;
				break;
			default:
				return false;  // keyboard/mouse/gamepad not yet supported
			}
			if (!pressed)
				return false;
		}
		return true;
	}
}

namespace ImGuiVRHelper
{
	HelperImpl& HelperImpl::GetSingleton()
	{
		static HelperImpl instance;
		return instance;
	}

	uint32_t HelperImpl::GetBuildNumber()
	{
		return IMGUI_VR_HELPER_BUILD_NUMBER;
	}

	uint32_t HelperImpl::RegisterClient(const char* name,
		ImGuiVRHelperPluginAPI::OnFrameFn on_frame, void* user, uint32_t flags)
	{
		if (!name || !on_frame) {
			logs::warn("RegisterClient: rejected (name={}, on_frame={})",
				name ? name : "<null>",
				static_cast<const void*>(on_frame));
			return 0;
		}

		std::scoped_lock lk{ m_mutex };
		const uint32_t id = m_next_client_id++;
		auto& rec = m_clients[id];
		rec.name = name;
		rec.on_frame = on_frame;
		rec.user = user;
		rec.flags = flags;

		logs::info("RegisterClient({}) -> client_id={} flags=0x{:x}", name, id, flags);
		return id;
	}

	void HelperImpl::UnregisterClient(uint32_t client_id)
	{
		std::scoped_lock lk{ m_mutex };
		if (auto it = m_clients.find(client_id); it != m_clients.end()) {
			logs::info("UnregisterClient({})  // {}", client_id, it->second.name);
			m_clients.erase(it);
		}
		// Drop combos owned by this client.
		for (auto it = m_combos.begin(); it != m_combos.end();) {
			if (it->second.client_id == client_id) {
				it = m_combos.erase(it);
			} else {
				++it;
			}
		}
		if (m_focused_client == client_id) {
			m_focused_client = 0;
		}
	}

	bool HelperImpl::EnsureClientTextureLocked(ClientRecord& rec)
	{
		if (rec.texture && rec.rtv) {
			return true;  // already allocated
		}
		if (!Globals::IsReady()) {
			return false;  // device not yet captured
		}
		auto& d3d = Globals::GetD3D();
		if (!d3d.device) {
			return false;
		}

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = static_cast<UINT>(Overlay::Config::kOverlayWidth);
		desc.Height = static_cast<UINT>(Overlay::Config::kOverlayHeight);
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = 0;

		winrt::com_ptr<ID3D11Texture2D> tex;
		HRESULT hr = d3d.device->CreateTexture2D(&desc, nullptr, tex.put());
		if (FAILED(hr)) {
			logs::warn("EnsureClientTextureLocked: CreateTexture2D failed (hr=0x{:x}) for client '{}'",
				static_cast<unsigned>(hr), rec.name);
			return false;
		}

		winrt::com_ptr<ID3D11RenderTargetView> rtv;
		hr = d3d.device->CreateRenderTargetView(tex.get(), nullptr, rtv.put());
		if (FAILED(hr)) {
			logs::warn("EnsureClientTextureLocked: CreateRenderTargetView failed (hr=0x{:x}) for client '{}'",
				static_cast<unsigned>(hr), rec.name);
			return false;
		}

		winrt::com_ptr<ID3D11ShaderResourceView> srv;
		hr = d3d.device->CreateShaderResourceView(tex.get(), nullptr, srv.put());
		if (FAILED(hr)) {
			logs::warn("EnsureClientTextureLocked: CreateShaderResourceView failed (hr=0x{:x}) for client '{}'",
				static_cast<unsigned>(hr), rec.name);
			return false;
		}

		rec.texture = std::move(tex);
		rec.rtv = std::move(rtv);
		rec.srv = std::move(srv);

		logs::info("Allocated panel texture for client '{}' ({}x{} RGBA8)",
			rec.name, desc.Width, desc.Height);
		return true;
	}

	bool HelperImpl::GetPanel(uint32_t client_id,
		ImGuiVRHelperPluginAPI::PanelHandle* out)
	{
		if (!out)
			return false;
		out->width = 0;
		out->height = 0;
		out->rtv = nullptr;

		std::scoped_lock lk{ m_mutex };
		auto it = m_clients.find(client_id);
		if (it == m_clients.end())
			return false;

		if (!EnsureClientTextureLocked(it->second)) {
			return false;
		}

		out->width = static_cast<uint32_t>(Overlay::Config::kOverlayWidth);
		out->height = static_cast<uint32_t>(Overlay::Config::kOverlayHeight);
		out->rtv = it->second.rtv.get();
		return true;
	}

	bool HelperImpl::GetPointer(uint32_t client_id, float* u, float* v,
		uint32_t* device_idx)
	{
		if (u)
			*u = 0.0f;
		if (v)
			*v = 0.0f;
		if (device_idx)
			*device_idx = 0;

		{
			std::scoped_lock lk{ m_mutex };
			if (!m_clients.contains(client_id))
				return false;
		}

		const auto& wand = Overlay::State::GetSingleton().wandState;
		if (!wand.isIntersecting)
			return false;

		if (u)
			*u = wand.uvCoordinates.x;
		if (v)
			*v = wand.uvCoordinates.y;
		if (device_idx)
			*device_idx = wand.controllerIndex;
		return true;
	}

	ImGuiVRHelperPluginAPI::ComboId HelperImpl::RegisterCombo(uint32_t client_id,
		const ImGuiVRHelperPluginAPI::InputCombo* keys, std::size_t n, float timeout_s)
	{
		if (!keys || n == 0) {
			return 0;
		}
		std::scoped_lock lk{ m_mutex };
		const auto id = m_next_combo_id++;
		auto& rec = m_combos[id];
		rec.client_id = client_id;
		rec.keys.assign(keys, keys + n);
		rec.timeout_s = timeout_s;
		rec.latched = false;
		logs::info("RegisterCombo(client={}, keys={}, timeout={}s) -> combo={}",
			client_id, n, timeout_s, id);
		return id;
	}

	bool HelperImpl::ComboFired(ImGuiVRHelperPluginAPI::ComboId combo_id)
	{
		std::scoped_lock lk{ m_mutex };
		auto it = m_combos.find(combo_id);
		if (it == m_combos.end()) {
			return false;
		}
		const bool fired = it->second.latched;
		it->second.latched = false;
		return fired;
	}

	void HelperImpl::StartComboRecording(uint32_t client_id, const char* label,
		ImGuiVRHelperPluginAPI::ComboRecordedFn on_done, void* user, float timeout_s)
	{
		logs::info("StartComboRecording(client={}, label={}, timeout={}s)",
			client_id, label ? label : "<null>", timeout_s);
		ComboRecording::Begin(client_id, label, on_done, user, timeout_s);
	}

	void HelperImpl::CancelComboRecording(uint32_t client_id)
	{
		ComboRecording::Cancel(client_id);
	}

	bool HelperImpl::IsOverlayVisible()
	{
		// In v1 the helper renders whenever a client is registered and
		// has focus. Show/hide is driven by client RequestFocus/ReleaseFocus.
		// A future revision can add a separate "user has the menu open"
		// gate driven by the helper's own toggle hotkey.
		return GetFocusedClientId() != 0;
	}

	ID3D11Texture2D* HelperImpl::GetClientPanelTexture(uint32_t client_id)
	{
		std::scoped_lock lk{ m_mutex };
		auto it = m_clients.find(client_id);
		if (it == m_clients.end())
			return nullptr;
		if (!EnsureClientTextureLocked(it->second))
			return nullptr;
		return it->second.texture.get();
	}

	uint32_t HelperImpl::GetFocusedClientId()
	{
		std::scoped_lock lk{ m_mutex };
		// Fall back to the first registered client when nobody has
		// explicitly requested focus. Avoids a "must call RequestFocus"
		// boilerplate on simple single-client setups.
		if (m_focused_client != 0 && m_clients.contains(m_focused_client)) {
			return m_focused_client;
		}
		if (!m_clients.empty()) {
			return m_clients.begin()->first;
		}
		return 0;
	}

	bool HelperImpl::IsSelfUIVisible() { return SettingsUI::IsVisible(); }

	uint32_t HelperImpl::GetSelfClientId() const { return m_self_client_id; }

	void HelperImpl::RebindSelfToggle(const ImGuiVRHelperPluginAPI::InputCombo* keys, std::size_t n)
	{
		if (!keys || n == 0)
			return;
		if (m_self_client_id == 0)
			return;
		// Drop the old combo by walking m_combos under the lock; replace
		// with a fresh registration so the matcher's was_matched state
		// resets (avoid spurious "fire" if user happens to be holding
		// any of the new keys at registration time).
		{
			std::scoped_lock lk{ m_mutex };
			m_combos.erase(m_self_toggle_combo);
		}
		m_self_toggle_combo = RegisterCombo(m_self_client_id, keys, n, 3.0f);
		logs::info("Rebound self-toggle combo to {} keys (combo_id={})",
			n, m_self_toggle_combo);
	}

	void HelperImpl::EnsureSelfClient()
	{
		if (m_self_client_id != 0)
			return;
		if (!Globals::IsReady())
			return;

		// Register a synthetic self-client. The on_frame callback is empty
		// because the helper drives its own rendering inline in
		// DispatchFrame (we don't need to round-trip through the public
		// callback path).
		m_self_client_id = RegisterClient(
			"ImGuiVRHelper.Settings",
			+[](const ImGuiVRHelperPluginAPI::Frame*, void*) { /* no-op */ },
			nullptr,
			ImGuiVRHelperPluginAPI::kClientFlag_None);

		// Default toggle combo: hold both grip buttons simultaneously.
		// Grip is rarely consumed by Skyrim's main menu (which uses face
		// buttons + trigger + stick), so it actually reaches our combo
		// matcher. The previous default (kBY-on-both) collided with the
		// main menu's BY = "back/cancel" handling and fired unreliably.
		// kGrip matches kGripAlt too (Input.cpp folds Oculus's axis-2
		// grip into the same button).
		// Users can also use the keyboard toggle (default F2) which
		// always reaches us regardless of menu state.
		using namespace ImGuiVRHelperPluginAPI;
		const InputCombo toggle_keys[] = {
			InputCombo(InputDeviceType::Both, RE::BSOpenVRControllerDevice::Keys::kGrip),
		};
		m_self_toggle_combo = RegisterCombo(m_self_client_id, toggle_keys, 1, 3.0f);

		logs::info("Self-client registered: id={} toggle_combo={} (grip+grip; F2 also toggles)",
			m_self_client_id, m_self_toggle_combo);
	}

	void HelperImpl::OnKeyboardToggle()
	{
		// Mirrors the controller-combo path in DispatchFrame: flip
		// visibility, then take focus on show / release on hide.
		SettingsUI::Toggle();
		const bool visible = SettingsUI::IsVisible();
		logs::info("Keyboard toggle (F2): settings UI now {}",
			visible ? "VISIBLE" : "hidden");
		if (m_self_client_id == 0)
			return;
		if (visible) {
			RequestFocus(m_self_client_id);
		} else if (m_focused_client == m_self_client_id) {
			ReleaseFocus(m_self_client_id);
		}
	}

	void HelperImpl::RequestFocus(uint32_t client_id)
	{
		std::scoped_lock lk{ m_mutex };
		if (m_clients.contains(client_id)) {
			m_focused_client = client_id;
		}
	}

	void HelperImpl::ReleaseFocus(uint32_t client_id)
	{
		std::scoped_lock lk{ m_mutex };
		if (m_focused_client == client_id) {
			m_focused_client = 0;
		}
	}

	void HelperImpl::TriggerHaptic(uint32_t /*client_id*/, uint32_t haptic_token,
		uint32_t duration_us, float /*frequency*/, float /*amplitude*/)
	{
		// haptic_token is (tracked_device_index + 1); 0 means "no haptic".
		// Frequency and amplitude are ignored — the legacy IVRSystem API
		// only supports duration. SteamVR Input would give finer control,
		// but that's a future API revision.
		if (haptic_token == 0)
			return;

		Util::OpenVRContext ctx;
		if (!ctx.IsValid())
			return;

		const auto deviceIndex = static_cast<vr::TrackedDeviceIndex_t>(haptic_token - 1);
		ctx.system->TriggerHapticPulse(deviceIndex, 0, static_cast<unsigned short>(std::min<uint32_t>(duration_us, 65535u)));
	}

	bool HelperImpl::ImportLegacySettings(const char* json_blob)
	{
		if (!json_blob)
			return false;
		try {
			auto j = nlohmann::json::parse(json_blob);
			// Merge whatever the legacy client gave us into Overlay::State's
			// settings. Each Settings field uses .value() with the current
			// in-memory default as fallback, so unknown / missing keys are
			// silently skipped.
			auto& s = Overlay::State::GetSingleton().settings;
			if (j.contains("VRMenuScale"))
				s.menuScale = j["VRMenuScale"];
			if (j.contains("VRMenuPositioningMethod"))
				s.positioningMethod = static_cast<Overlay::PositioningMethod>(
					j["VRMenuPositioningMethod"].get<int>());
			if (j.contains("attachMode"))
				s.attachMode = static_cast<Overlay::AttachMode>(
					j["attachMode"].get<int>());
			if (j.contains("VRMenuAttachController"))
				s.attachController = static_cast<ImGuiVRHelperPluginAPI::InputDeviceType>(
					j["VRMenuAttachController"].get<int>());
			if (j.contains("VRMenuOffsetX"))
				s.hmdOffsetX = j["VRMenuOffsetX"];
			if (j.contains("VRMenuOffsetY"))
				s.hmdOffsetY = j["VRMenuOffsetY"];
			if (j.contains("VRMenuOffsetZ"))
				s.hmdOffsetZ = j["VRMenuOffsetZ"];
			if (j.contains("VRMenuControllerOffsetX"))
				s.controllerOffsetX = j["VRMenuControllerOffsetX"];
			if (j.contains("VRMenuControllerOffsetY"))
				s.controllerOffsetY = j["VRMenuControllerOffsetY"];
			if (j.contains("VRMenuControllerOffsetZ"))
				s.controllerOffsetZ = j["VRMenuControllerOffsetZ"];
			if (j.contains("EnableWandPointing"))
				s.enableWandPointing = j["EnableWandPointing"];
			if (j.contains("EnableDragToReposition"))
				s.enableDragToReposition = j["EnableDragToReposition"];
			if (j.contains("VRMenuAutoResetDistance"))
				s.autoResetDistance = j["VRMenuAutoResetDistance"];
			if (j.contains("mouseDeadzone"))
				s.mouseDeadzone = j["mouseDeadzone"];
			Overlay::SaveSettings();
			logs::info("ImportLegacySettings: merged {} keys, persisted to disk", j.size());
			return true;
		} catch (const std::exception& e) {
			logs::warn("ImportLegacySettings parse error: {}", e.what());
			return false;
		}
	}

	void HelperImpl::FeedVREvent(uint32_t device, uint32_t key_code, bool pressed,
		float thumbstick_x, float thumbstick_y)
	{
		Input::FeedVREvent(device, key_code, pressed, thumbstick_x, thumbstick_y);
	}

	void HelperImpl::DispatchFrame(float dt)
	{
		// Update the wand-pointer intersection once per tick. The result
		// lives on Overlay::State::wandState and is consumed by GetPointer
		// (per-client) and by Frame.flags bit 2 (client_pointer_in_panel).
		auto& overlayState = Overlay::State::GetSingleton();
		const auto& s = overlayState.settings;
		if (s.enableWandPointing) {
			namespace API = ImGuiVRHelperPluginAPI;
			// Pointer hand: opposite of attached when controller-attached;
			// otherwise the primary hand.
			API::InputDeviceType pointer;
			if (s.attachMode == Overlay::AttachMode::ControllerOnly ||
				s.attachMode == Overlay::AttachMode::Both) {
				pointer = (s.attachController == API::InputDeviceType::Primary) ?
				              API::InputDeviceType::Secondary :
				              API::InputDeviceType::Primary;
			} else {
				pointer = API::InputDeviceType::Primary;
			}
			const auto idx = Util::GetControllerIndexForDevice(
				pointer, overlayState.lastKnownLeftHandedMode);
			if (idx != vr::k_unTrackedDeviceIndexInvalid) {
				ImVec2 uv;
				WandPointing::ComputeIntersection(idx, uv);
			} else {
				overlayState.wandState.isIntersecting = false;
			}
		} else {
			overlayState.wandState.isIntersecting = false;
		}

		ImGuiVRHelperPluginAPI::Frame baseFrame;
		Input::BuildFrame(baseFrame, dt);

		// Drag state machine: lazy-init the fixed-world transform on first
		// visible frame, run grip-to-drag if active, auto-reset when the
		// player travels far enough.
		OverlayDrag::UpdateFixedWorldPositioning();
		OverlayDrag::Update();

		// Combo recording: detect press/release edges, accumulate the
		// recorded combo, deliver via callback when done. While active
		// the helper takes self-focus so the modal is visible.
		ComboRecording::Tick(dt);
		if (ComboRecording::IsActive() && m_self_client_id != 0) {
			RequestFocus(m_self_client_id);
		}

		// Self-toggle combo (default: Both grip buttons). When fired,
		// flip the helper's settings UI visibility. The combo machinery
		// latches rising edges, so this fires once per held cycle.
		if (m_self_toggle_combo != 0 && ComboFired(m_self_toggle_combo)) {
			SettingsUI::Toggle();
			const bool visible = SettingsUI::IsVisible();
			logs::info("Controller toggle combo: settings UI now {}",
				visible ? "VISIBLE" : "hidden");
			if (visible && m_self_client_id != 0) {
				RequestFocus(m_self_client_id);
			} else if (m_self_client_id != 0 && m_focused_client == m_self_client_id) {
				ReleaseFocus(m_self_client_id);
			}
		}

		// Combo matcher: walk all registered combos, latch rising edges.
		// Held-but-already-matched combos do NOT re-fire — clients see one
		// "fired" event per held cycle.
		{
			std::scoped_lock lk{ m_mutex };
			for (auto& [id, combo] : m_combos) {
				const bool matched = MatchCombo(combo.keys);
				if (matched && !combo.was_matched) {
					combo.latched = true;
				}
				combo.was_matched = matched;
			}
		}

		// Snapshot the client list under the lock so we don't hold the
		// mutex across a callback into client code (avoids deadlocks if a
		// client calls back into RegisterClient/UnregisterClient).
		struct Snapshot
		{
			uint32_t id;
			ImGuiVRHelperPluginAPI::OnFrameFn on_frame;
			void* user;
			uint32_t flags;
		};
		std::vector<Snapshot> snapshot;
		uint32_t focused;
		{
			std::scoped_lock lk{ m_mutex };
			focused = m_focused_client;
			snapshot.reserve(m_clients.size());
			for (const auto& [id, rec] : m_clients) {
				snapshot.push_back({ id, rec.on_frame, rec.user, rec.flags });
			}
		}

		const bool wandHit = overlayState.wandState.isIntersecting;

		// Render the helper's own settings UI into its self-client panel
		// before the client OnFrame loop, so InSceneOverlay's per-eye
		// composite picks up the latest pixels.
		if (m_self_client_id != 0 && SettingsUI::IsVisible()) {
			ImGuiVRHelperPluginAPI::PanelHandle handle{};
			if (GetPanel(m_self_client_id, &handle) && handle.rtv) {
				if (SettingsUI::Render(dt)) {
					auto* ctx = Globals::GetD3D().context;
					if (ctx) {
						ID3D11RenderTargetView* oldRTV = nullptr;
						ID3D11DepthStencilView* oldDSV = nullptr;
						ctx->OMGetRenderTargets(1, &oldRTV, &oldDSV);
						const float clear[4] = { 0, 0, 0, 0 };
						ctx->OMSetRenderTargets(1, &handle.rtv, nullptr);
						ctx->ClearRenderTargetView(handle.rtv, clear);
						ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
						ctx->OMSetRenderTargets(1, &oldRTV, oldDSV);
						if (oldRTV)
							oldRTV->Release();
						if (oldDSV)
							oldDSV->Release();
					}
				}
			}
		}

		for (const auto& sn : snapshot) {
			ImGuiVRHelperPluginAPI::Frame perClient = baseFrame;
			if (sn.id == focused) {
				perClient.flags |= 1u << 0;  // bit0 client_has_focus
				if (wandHit) {
					perClient.flags |= 1u << 2;  // bit2 client_pointer_in_panel
				}
			} else {
				perClient.flags &= ~((1u << 0) | (1u << 2));
			}

			if (sn.on_frame) {
				sn.on_frame(&perClient, sn.user);
			}
		}

		// Compute-shader tinting: copy focused client's panel through the
		// OverlayTinter (with drag-highlight color when dragging) into a
		// helper-owned post-process texture. OverlayManager hands SteamVR
		// the tinted output when dragging, the raw panel otherwise.
		// Skip when nothing's focused — saves a dispatch.
		if (focused != 0) {
			if (auto* tex = GetClientPanelTexture(focused)) {
				const bool dragging = overlayState.dragState.dragging &&
				                      overlayState.settings.enableDragToReposition;
				const float tint[4] = {
					dragging ? overlayState.settings.dragHighlightColor[0] : 0.0f,
					dragging ? overlayState.settings.dragHighlightColor[1] : 0.0f,
					dragging ? overlayState.settings.dragHighlightColor[2] : 0.0f,
					dragging ? overlayState.settings.dragHighlightColor[3] : 0.0f,
				};
				OverlayTinter::Dispatch(tex, tint);
			}
		}

		// Hand the focused client's panel to SteamVR via IVROverlay. Must
		// run AFTER per-client OnFrame (so panel pixels are current) and
		// AFTER OverlayTinter::Dispatch (so the tinted version exists when
		// dragging). OverlayManager handles its own lazy init and the
		// hide/show decision based on attachMode + focus.
		OverlayManager::Tick();

		// Keep Overlay::State::overlayVisible in sync with focus state so
		// OverlayDrag::CanPerform sees the right value next frame.
		overlayState.overlayVisible = (focused != 0);
	}
}
