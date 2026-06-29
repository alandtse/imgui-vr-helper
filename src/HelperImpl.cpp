// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Implementation of IImGuiVRHelperInterface001: the client registry, per-client
// panel textures, combo matching/recording, focus, and per-frame dispatch.

#include "pch.h"

#include "ComboRecording.h"
#include "Globals.h"
#include "HUDDemo.h"
#include "HelperImpl.h"
#include "HudDisplay.h"
#include "HudRender.h"
#include "Input.h"
#include "Overlay.h"
#include "OverlayDrag.h"
#include "OverlayTinter.h"
#include "PluginVersion.h"
#include "SettingsUI.h"
#include "ToastHUD.h"
#include "VRKeyboard.h"
#include "WandPointing.h"
#include "internal/Profiler.h"
#include "internal/VRUtils.h"

#include <RE/B/BSOpenVRControllerDevice.h>
#include <RE/U/UI.h>

#include <nlohmann/json.hpp>

namespace
{
	// on_rebind for the helper-owned open/close combos: persist the new chord to
	// settings so a rebind survives a restart.
	void PersistOpenMenuKeys(const ImGuiVRHelperPluginAPI::InputCombo* keys, std::size_t n, void*)
	{
		ImGuiVRHelper::Overlay::State::GetSingleton().settings.openMenuKeys.assign(keys, keys + n);
		ImGuiVRHelper::Overlay::SaveSettings();
	}
	void PersistCloseMenuKeys(const ImGuiVRHelperPluginAPI::InputCombo* keys, std::size_t n, void*)
	{
		ImGuiVRHelper::Overlay::State::GetSingleton().settings.closeMenuKeys.assign(keys, keys + n);
		ImGuiVRHelper::Overlay::SaveSettings();
	}

	// Registration name of the helper's own settings/toast client. Shared so the
	// panel-sizing supersample check stays in sync with EnsureSelfClient.
	constexpr const char* kSelfClientName = "ImGuiVRHelper.Settings";

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

	// True iff an overlay may be OPENED right now. When the user keeps the
	// "only open while paused" setting on (the default — matches Community
	// Shaders' original pause-only menu), opening an overlay from nothing is
	// blocked during live gameplay so menus never affect it. Once something is
	// already shown (currentFocus != 0), switching/closing stays allowed.
	bool OverlayOpenAllowed(uint32_t currentFocus)
	{
		const auto& s = ImGuiVRHelper::Overlay::State::GetSingleton().settings;
		if (!s.onlyOpenWhilePaused)
			return true;
		if (currentFocus != 0)
			return true;
		auto* ui = RE::UI::GetSingleton();
		return ui && ui->GameIsPaused();
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

	uint32_t HelperImpl::RegisterClient(const char* name, const char* version,
		ImGuiVRHelperPluginAPI::OnFrameFn on_frame, void* user, uint32_t flags)
	{
		if (!name) {
			logs::warn("RegisterClient: rejected (null name)");
			return 0;
		}
		// on_frame may be null: HUD-mode and poll-based clients don't need the
		// per-frame Frame callback (DispatchToClients null-checks before
		// invoking it). The self-client passes an empty callback for the same
		// reason; external clients can now simply pass nullptr.

		uint32_t effective_flags = flags;
		// Strip the deprecated dashboard flag so it doesn't propagate internally.
		effective_flags &= ~ImGuiVRHelperPluginAPI::kClientFlag_Dashboard;

		std::scoped_lock lk{ m_mutex };
		const uint32_t id = m_next_client_id++;
		auto& rec = m_clients[id];
		rec.name = name;
		rec.version = version ? version : "";
		rec.on_frame = on_frame;
		rec.user = user;
		rec.flags = effective_flags;

		if (rec.version.empty()) {
			logs::info("RegisterClient({}) -> client_id={} flags=0x{:x}",
				name, id, effective_flags);
		} else {
			logs::info("RegisterClient({} v{}) -> client_id={} flags=0x{:x}",
				name, rec.version, id, effective_flags);
		}
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

	void HelperImpl::PanelPixelSize(const ClientRecord& rec, unsigned int& width, unsigned int& height) const
	{
		namespace API = ImGuiVRHelperPluginAPI;
		int baseW, baseH;
		ResolveBaseDims(baseW, baseH);
		// The self/settings panel isn't HUD-mode (it's focusable), but it fills the
		// view and carries the toasts, so it's supersampled too.
		const bool fillsView = (rec.flags & API::kClientFlag_HUDMode) || rec.name == kSelfClientName;
		const int ss = fillsView ? HudSupersample() : 1;
		width = static_cast<unsigned int>(baseW * ss);
		height = static_cast<unsigned int>(baseH * ss);
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

		unsigned int pw = 0, ph = 0;
		PanelPixelSize(rec, pw, ph);

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = static_cast<UINT>(pw);
		desc.Height = static_cast<UINT>(ph);
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

		// Stamp the access so the HUD pass knows this client painted this frame
		// (idle HUD layers are skipped to save the always-on composite cost).
		it->second.lastPanelFrame = m_frameCounter;

		// Report the real (base × supersample) panel size so view-filling clients
		// can scale their framebuffer to match — see EnsureClientTextureLocked.
		unsigned int pw = 0, ph = 0;
		PanelPixelSize(it->second, pw, ph);
		out->width = pw;
		out->height = ph;
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
		const ImGuiVRHelperPluginAPI::InputCombo* keys, std::size_t n, float timeout_s,
		const char* label, ImGuiVRHelperPluginAPI::ComboRebindFn on_rebind, void* user)
	{
		if (!keys || n == 0) {
			return 0;
		}
		// Reject malformed combos rather than storing one that can never fire:
		// an absurd key count, or a key on an input device the matcher can't
		// read (only the VR controllers are supported — see MatchCombo).
		constexpr std::size_t kMaxComboKeys = 8;
		if (n > kMaxComboKeys) {
			logs::warn("RegisterCombo(client={}): {} keys exceeds max {}; rejected",
				client_id, n, kMaxComboKeys);
			return 0;
		}
		namespace API = ImGuiVRHelperPluginAPI;
		for (std::size_t i = 0; i < n; ++i) {
			const auto dev = keys[i].GetDevice();
			if (dev != API::InputDeviceType::Primary &&
				dev != API::InputDeviceType::Secondary &&
				dev != API::InputDeviceType::Both) {
				logs::warn("RegisterCombo(client={}): unsupported input device {}; rejected",
					client_id, static_cast<int>(dev));
				return 0;
			}
		}
		std::scoped_lock lk{ m_mutex };
		const auto id = m_next_combo_id++;
		auto& rec = m_combos[id];
		rec.client_id = client_id;
		rec.label = label ? label : "";
		rec.keys.assign(keys, keys + n);
		rec.timeout_s = timeout_s;
		rec.latched = false;
		rec.on_rebind = on_rebind;
		rec.on_rebind_user = user;
		logs::info("RegisterCombo(client={}, keys={}, timeout={}s) -> combo={}",
			client_id, n, timeout_s, id);
		return id;
	}

	void HelperImpl::RebindCombo(ImGuiVRHelperPluginAPI::ComboId combo,
		const ImGuiVRHelperPluginAPI::InputCombo* keys, std::size_t n)
	{
		// n == 0 clears the binding (unbind) — keys may be null. A non-empty
		// rebind needs valid keys and a sane chord length. The matcher already
		// treats an empty key set as never-fires, so a cleared combo is inert.
		if (n > 8 || (n > 0 && !keys))
			return;
		ImGuiVRHelperPluginAPI::ComboRebindFn cb = nullptr;
		void* user = nullptr;
		{
			std::scoped_lock lk{ m_mutex };
			auto it = m_combos.find(combo);
			if (it == m_combos.end())
				return;
			// Update in place — same ComboId so the owning client keeps polling
			// the same handle. Reset match state so a key held during recording
			// doesn't fire a spurious activation.
			it->second.keys.assign(keys, keys + n);
			it->second.was_matched = false;
			it->second.latched = false;
			cb = it->second.on_rebind;
			user = it->second.on_rebind_user;
		}
		// Notify the owning client OUTSIDE the lock so it can persist the new
		// binding (and is free to call back into the API).
		if (cb)
			cb(keys, n, user);
		logs::info("RebindCombo(combo={}) -> {} keys", combo, n);
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

	void HelperImpl::SetComboOffPanel(ImGuiVRHelperPluginAPI::ComboId combo, bool off_panel)
	{
		std::scoped_lock lk{ m_mutex };
		if (auto it = m_combos.find(combo); it != m_combos.end()) {
			it->second.off_panel = off_panel;
		}
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

	ID3D11Texture2D* HelperImpl::GetActiveRebindTexture()
	{
		if (m_rebind_client_id == 0 || !ComboRecording::IsActive())
			return nullptr;
		return GetClientPanelTexture(m_rebind_client_id);
	}

	void HelperImpl::SetHudForceDisabled(uint32_t client_id, bool disabled)
	{
		std::scoped_lock lk{ m_mutex };
		if (auto it = m_clients.find(client_id); it != m_clients.end())
			it->second.hudForceDisabled = disabled;
	}

	std::vector<HelperImpl::ComboSnapshot> HelperImpl::SnapshotCombos()
	{
		std::vector<ComboSnapshot> out;
		{
			std::scoped_lock lk{ m_mutex };
			out.reserve(m_combos.size());
			for (const auto& [id, rec] : m_combos) {
				ComboSnapshot s{};
				s.combo_id = id;
				s.client_id = rec.client_id;
				if (auto it = m_clients.find(rec.client_id); it != m_clients.end())
					s.client_name = it->second.name;
				s.label = rec.label;
				s.keys = rec.keys;
				s.conflict = false;
				s.off_panel = rec.off_panel;
				out.push_back(std::move(s));
			}
		}
		// Flag identical chords (same set of device+key) so the UI can warn when
		// two combos clash. O(n^2) but n is tiny (a handful of combos).
		const auto sameChord = [](const std::vector<ImGuiVRHelperPluginAPI::InputCombo>& a,
								   const std::vector<ImGuiVRHelperPluginAPI::InputCombo>& b) {
			if (a.empty() || a.size() != b.size())
				return false;
			for (const auto& k : a) {
				bool found = false;
				for (const auto& k2 : b)
					if (k2.GetDevice() == k.GetDevice() && k2.GetKey() == k.GetKey()) {
						found = true;
						break;
					}
				if (!found)
					return false;
			}
			return true;
		};
		// Any two registered combos sharing a chord clash off-panel (an off-panel combo is active
		// off the panel; a global one is active everywhere), so flag every chord match. On-panel UI
		// buttons aren't registered combos, so they're never part of this pass.
		for (size_t i = 0; i < out.size(); ++i)
			for (size_t j = i + 1; j < out.size(); ++j)
				if (sameChord(out[i].keys, out[j].keys)) {
					out[i].conflict = true;
					out[j].conflict = true;
				}
		std::sort(out.begin(), out.end(), [](const ComboSnapshot& a, const ComboSnapshot& b) {
			if (a.client_id != b.client_id)
				return a.client_id < b.client_id;
			return a.combo_id < b.combo_id;
		});
		return out;
	}

	std::vector<HelperImpl::ClientSnapshot> HelperImpl::SnapshotClients()
	{
		std::vector<ClientSnapshot> out;
		std::scoped_lock lk{ m_mutex };
		out.reserve(m_clients.size());
		for (const auto& [id, rec] : m_clients) {
			ClientSnapshot snap{};
			snap.client_id = id;
			snap.name = rec.name;
			snap.version = rec.version;
			snap.flags = rec.flags;
			snap.has_texture = (rec.texture != nullptr);
			snap.has_focus = (m_focused_client == id);
			const bool isHud = (rec.flags & ImGuiVRHelperPluginAPI::kClientFlag_HUDMode) != 0;
			snap.hud_force_disabled = rec.hudForceDisabled;
			snap.hud_compositing = isHud && !rec.hudForceDisabled && rec.lastPanelFrame != 0 &&
			                       (m_frameCounter - rec.lastPanelFrame) <= 2;  // matches kHudIdleFrames
			out.push_back(std::move(snap));
		}
		// Stable order by client_id so the list doesn't reshuffle
		// frame-to-frame when the underlying unordered_map rehashes.
		std::sort(out.begin(), out.end(),
			[](const ClientSnapshot& a, const ClientSnapshot& b) {
				return a.client_id < b.client_id;
			});
		return out;
	}

	std::vector<HelperImpl::HUDClientSnapshot> HelperImpl::SnapshotHUDClients()
	{
		std::vector<HUDClientSnapshot> out;
		std::scoped_lock lk{ m_mutex };
		// Reserve once. Most setups have 0–1 HUD clients; bumping
		// reserve to 4 covers the common multi-HUD case (subtitles +
		// damage numbers + radar + ...) without reallocating.
		out.reserve(4);
		// Composite a HUD layer only if it painted within the last few frames —
		// an idle layer (toast dismissed, welcome gone, demo off) would otherwise
		// cost a full-FOV alpha blend per eye for zero pixels. Always-on during
		// gameplay, so this keeps idle HUD cost at zero.
		constexpr uint64_t kHudIdleFrames = 2;
		for (auto& [id, rec] : m_clients) {
			if ((rec.flags & ImGuiVRHelperPluginAPI::kClientFlag_HUDMode) == 0)
				continue;
			if (id == m_rebind_client_id)
				continue;  // rebind overlay draws in its own on-top pass, not the HUD pass
			if (rec.hudForceDisabled)
				continue;  // debug: layer hidden to isolate others
			if (rec.lastPanelFrame == 0 || (m_frameCounter - rec.lastPanelFrame) > kHudIdleFrames)
				continue;  // never painted, or idle → skip the composite
			if (!EnsureClientTextureLocked(rec))
				continue;
			out.push_back({ id, rec.texture.get() });
		}
		return out;
	}

	uint32_t HelperImpl::GetFocusedClientId()
	{
		std::scoped_lock lk{ m_mutex };
		// 0 means "nobody is focused, do not display anything." Earlier
		// versions fell back to the first registered client to save
		// boilerplate, but that broke the helper's own toggle: closing
		// the settings UI calls ReleaseFocus(self) -> m_focused_client = 0,
		// and the fallback would then re-elect self -> OverlayManager
		// kept calling ShowOverlay -> the menu stayed on screen forever.
		// Clients (including the helper's self-client) MUST call
		// RequestFocus to be displayed.
		if (m_focused_client != 0 && m_clients.contains(m_focused_client)) {
			return m_focused_client;
		}
		return 0;
	}

	std::string HelperImpl::GetClientVersion(uint32_t client_id)
	{
		std::scoped_lock lk{ m_mutex };
		if (client_id == m_self_client_id)
			return IMGUI_VR_HELPER_VERSION_STRING;
		auto it = m_clients.find(client_id);
		if (it != m_clients.end()) {
			return it->second.version;
		}
		return "";
	}

	bool HelperImpl::IsSelfUIVisible() { return SettingsUI::IsVisible(); }

	bool HelperImpl::ShouldSwallowInput() const
	{
		// Swallow VR controller input from the game whenever an interactive
		// overlay is up: our settings window, the combo-recording modal, or a
		// focused client panel (e.g. a swapped-to client menu). m_focused_client
		// is only ever a panel overlay (HUD clients are never focused), so this
		// is exactly "an interactive overlay wants the input."
		if (SettingsUI::IsVisible() || ComboRecording::IsActive()) {
			return true;
		}
		if (m_focused_client == 0) {
			return false;
		}
		// A pointer-focus client coexists with the game's own menu: it owns the
		// wand only while the laser is on its panel, so the underlying menu (e.g.
		// the dialogue menu during a conversation) keeps input every other frame.
		const auto it = m_clients.find(m_focused_client);
		if (it != m_clients.end() &&
			(it->second.flags & ImGuiVRHelperPluginAPI::kClientFlag_PointerFocus) != 0) {
			return Overlay::State::GetSingleton().wandState.isIntersecting;
		}
		return true;
	}

	bool HelperImpl::IsLiveToolFocused() const
	{
		// Only a focused client can be a live tool; the helper's own settings UI
		// and the combo modal are always pause-time, so they never qualify.
		if (m_focused_client == 0 || SettingsUI::IsVisible() || ComboRecording::IsActive()) {
			return false;
		}
		const auto it = m_clients.find(m_focused_client);
		return it != m_clients.end() &&
		       (it->second.flags & ImGuiVRHelperPluginAPI::kClientFlag_LiveTool) != 0;
	}

	void HelperImpl::NotifyEnteredGame()
	{
		m_enteredGame = true;  // one-way latch; dismisses the startup welcome
	}

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
		m_self_toggle_combo = RegisterCombo(m_self_client_id, keys, n, 3.0f, "Open ImGuiVRHelper settings", nullptr, nullptr);
		logs::info("Rebound self-toggle combo to {} keys (combo_id={})",
			n, m_self_toggle_combo);
	}

	void HelperImpl::RebindOpenMenu(const ImGuiVRHelperPluginAPI::InputCombo* keys, std::size_t n)
	{
		if (!keys || n == 0 || m_self_client_id == 0)
			return;
		{
			std::scoped_lock lk{ m_mutex };
			m_combos.erase(m_open_menu_combo);
		}
		// Re-register (resets the matcher's was_matched state) keeping the persist
		// callback, then mirror into Settings — RegisterCombo doesn't fire on_rebind.
		m_open_menu_combo = RegisterCombo(m_self_client_id, keys, n, 3.0f, "Open menu", PersistOpenMenuKeys, nullptr);
		Overlay::State::GetSingleton().settings.openMenuKeys.assign(keys, keys + n);
		Overlay::SaveSettings();
		logs::info("Rebound open-menu combo to {} keys (combo_id={})", n, m_open_menu_combo);
	}

	void HelperImpl::RebindCloseMenu(const ImGuiVRHelperPluginAPI::InputCombo* keys, std::size_t n)
	{
		if (!keys || n == 0 || m_self_client_id == 0)
			return;
		{
			std::scoped_lock lk{ m_mutex };
			m_combos.erase(m_close_menu_combo);
		}
		m_close_menu_combo = RegisterCombo(m_self_client_id, keys, n, 3.0f, "Close menu", PersistCloseMenuKeys, nullptr);
		Overlay::State::GetSingleton().settings.closeMenuKeys.assign(keys, keys + n);
		Overlay::SaveSettings();
		logs::info("Rebound close-menu combo to {} keys (combo_id={})", n, m_close_menu_combo);
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
		// RendersOnFocus: the helper unconditionally renders its
		// settings UI into the self-client RTV (DispatchFrame's
		// SettingsUI::Render block), so it trivially honors the
		// focus-render contract.
		m_self_client_id = RegisterClient(
			kSelfClientName,
			nullptr,
			+[](const ImGuiVRHelperPluginAPI::Frame*, void*) { /* no-op */ },
			nullptr,
			ImGuiVRHelperPluginAPI::kClientFlag_RendersOnFocus);

		// Synthetic HUD-mode client for the Settings::showHUDDemo smoke
		// test. Always registered (zero overhead until showHUDDemo
		// flips on — texture allocation is lazy via SnapshotHUDClients).
		// Lets a user verify kClientFlag_HUDMode end-to-end before any
		// real HUD client (e.g. a VR-port subtitle mod) connects.
		m_hud_demo_client_id = RegisterClient(
			"ImGuiVRHelper.HUDDemo",
			nullptr,
			+[](const ImGuiVRHelperPluginAPI::Frame*, void*) { /* no-op */ },
			nullptr,
			ImGuiVRHelperPluginAPI::kClientFlag_HUDMode);

		// HUD-mode client for the overlay-swap toast (see m_toastText). Always
		// registered; texture allocation stays lazy until the first toast.
		m_toast_client_id = RegisterClient(
			"ImGuiVRHelper.Toast",
			nullptr,
			+[](const ImGuiVRHelperPluginAPI::Frame*, void*) { /* no-op */ },
			nullptr,
			ImGuiVRHelperPluginAPI::kClientFlag_HUDMode);

		// HUD-mode client for the startup welcome banner (see m_welcome*).
		m_welcome_client_id = RegisterClient(
			"ImGuiVRHelper.Welcome",
			nullptr,
			+[](const ImGuiVRHelperPluginAPI::Frame*, void*) { /* no-op */ },
			nullptr,
			ImGuiVRHelperPluginAPI::kClientFlag_HUDMode);

		// Panel for the combo-rebind capture overlay. HUD-mode keeps it out of
		// the cycle order; SnapshotHUDClients skips it so it isn't drawn in the
		// head-locked HUD pass — InSceneOverlay composites it on top of the
		// focused client instead (so the capture reads as a modal over the menu).
		m_rebind_client_id = RegisterClient(
			"ImGuiVRHelper.Rebind",
			nullptr,
			+[](const ImGuiVRHelperPluginAPI::Frame*, void*) { /* no-op */ },
			nullptr,
			ImGuiVRHelperPluginAPI::kClientFlag_HUDMode);

		// Take over open/close of the active overlay: the helper owns these
		// combos (registered on the self-client) so the welcome banner can
		// document them and activation is consistent across clients. Keys load
		// from settings; rebinds persist via their on_rebind.
		{
			const auto& s = Overlay::State::GetSingleton().settings;
			if (!s.openMenuKeys.empty())
				m_open_menu_combo = RegisterCombo(m_self_client_id, s.openMenuKeys.data(),
					s.openMenuKeys.size(), 0.0f, "Open menu", &PersistOpenMenuKeys, nullptr);
			if (!s.closeMenuKeys.empty())
				m_close_menu_combo = RegisterCombo(m_self_client_id, s.closeMenuKeys.data(),
					s.closeMenuKeys.size(), 0.0f, "Close menu", &PersistCloseMenuKeys, nullptr);
		}

		// No default controller toggle combo. SCS already binds many of
		// the obvious face/grip combinations for its own menu, and every
		// auto-default we've tried has bumped into a SCS or main-menu
		// conflict (kBY-Both = main-menu back/cancel; kGrip-Both
		// shadowed drag-to-reposition; Secondary kBY hijacked SCS's
		// VRMenuOpenKeys). Default toggle is keyboard-only (Shift+F4)
		// so users on a stock install can't accidentally collide with
		// their other mods. Users who want a controller binding open
		// the settings via Shift+F4 once and then click 'Rebind toggle
		// key' to record any combo they want via ComboRecording.
		logs::info("Self-client registered: id={} (toggle: Shift+F4; bind controller combo via settings)",
			m_self_client_id);
	}

	void HelperImpl::OnKeyboardToggle()
	{
		// Intentionally NOT pause-gated — keyboard is a desktop/dev convenience;
		// only controller activation is gated (see the self-toggle combo).
		SettingsUI::Toggle();
		logs::info("Keyboard toggle (Shift+F4): settings UI now {}",
			SettingsUI::IsVisible() ? "VISIBLE" : "hidden");
		// Focus + SaveSettings handled by SyncSelfFocus reconciler
		// in DispatchFrame — every close path goes through the same
		// single source of truth.
	}

	void HelperImpl::RequestFocus(uint32_t client_id)
	{
		// NOT pause-gated: RequestFocus is reached by keyboard-driven opens
		// (the helper's Shift+F4 and a client's own keyboard hotkey), which are
		// desktop/dev conveniences. The pause gate applies only to controller
		// activation — see the self-toggle combo in ReconcileSelfFocusAndCombos.
		std::scoped_lock lk{ m_mutex };
		if (m_clients.contains(client_id)) {
			m_focused_client = client_id;
			// Remember the last client overlay so the open combo can reopen it.
			// The helper's own settings UI has its own toggle, so it's excluded.
			if (client_id != m_self_client_id)
				m_lastOpenedOverlay = client_id;
		}
	}

	void HelperImpl::ReleaseFocus(uint32_t client_id)
	{
		std::scoped_lock lk{ m_mutex };
		if (m_focused_client == client_id) {
			m_focused_client = 0;
		}
	}

	uint32_t HelperImpl::PickOpenTarget()
	{
		{
			std::scoped_lock lk{ m_mutex };
			if (m_lastOpenedOverlay != 0 && m_clients.contains(m_lastOpenedOverlay))
				return m_lastOpenedOverlay;
		}
		// Cold start / stale last-opened: first non-helper overlay in cycle order.
		for (const auto& [id, name] : BuildOverlayOrder()) {
			if (id != m_self_client_id)
				return id;
		}
		return 0;
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

	// Wand-pointer intersection for the tick. Result lives on
	// Overlay::State::wandState; consumed by GetPointer and Frame flag bit 2.
	void HelperImpl::UpdateWandPointer()
	{
		auto& overlayState = Overlay::State::GetSingleton();
		const auto& s = overlayState.settings;
		if (!s.enableWandPointing) {
			overlayState.wandState.isIntersecting = false;
			return;
		}

		namespace API = ImGuiVRHelperPluginAPI;
		// Pointer hand: opposite of the attached hand when controller-attached,
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
	}

	// Reconcile self-UI focus and latch combo rising edges. Order matters:
	// runs after ComboRecording::Tick (so IsActive() reflects this frame).
	void HelperImpl::ReconcileSelfFocusAndCombos()
	{
		// Self-toggle combo (bound via "Rebind toggle"; default is Shift+F4 in
		// OnKeyboardToggle). Latches rising edges → fires once per held cycle.
		if (m_self_toggle_combo != 0 && ComboFired(m_self_toggle_combo)) {
			// Gate opening on pause (closing is always allowed).
			if (SettingsUI::IsVisible() || OverlayOpenAllowed(m_focused_client)) {
				SettingsUI::Toggle();
				logs::info("Controller toggle combo: settings UI now {}",
					SettingsUI::IsVisible() ? "VISIBLE" : "hidden");
			} else {
				logs::info("Controller toggle combo ignored: game not paused");
			}
		}

		// Helper-owned open/close of the active overlay (activation taken over
		// from clients). Open focuses the last-opened overlay (or the first mod
		// overlay) when nothing is up — pause-gated like the toggle; close
		// releases focus. Switching between open overlays is the cycle shortcut's
		// job, so open is a no-op while one is already focused.
		if (m_open_menu_combo != 0 && ComboFired(m_open_menu_combo)) {
			if (m_focused_client == 0 && OverlayOpenAllowed(0)) {
				if (const uint32_t target = PickOpenTarget(); target != 0) {
					RequestFocus(target);
					logs::info("Open-menu combo: focusing overlay {}", target);
				} else if (m_self_client_id != 0) {
					// No mod overlay to open — fall back to the helper's own settings
					// so a controller-only user can always reach the menu (and the
					// overlay launcher) without a keyboard.
					SettingsUI::SetVisible(true);
					logs::info("Open-menu combo: no overlay; opening helper settings");
				}
			}
		}
		if (m_close_menu_combo != 0 && ComboFired(m_close_menu_combo)) {
			if (m_self_client_id != 0 && m_focused_client == m_self_client_id) {
				// The helper's own settings UI is gated on visibility, not focus —
				// releasing focus alone would be re-grabbed by the reconciler
				// below. Hide it; the reconciler then releases focus and persists.
				SettingsUI::SetVisible(false);
				logs::info("Close-menu combo: hiding helper settings UI");
			} else if (m_focused_client != 0) {
				logs::info("Close-menu combo: releasing overlay {}", m_focused_client);
				ReleaseFocus(m_focused_client);
			}
		}

		// Single source of truth for "self UI active → self-client holds focus".
		// Runs after every path that can flip SettingsUI visibility this frame
		// (hotkey, combo, ImGui X-button); otherwise a stale focus would keep
		// InSceneOverlay painting the closed panel to both eyes.
		if (m_self_client_id != 0) {
			// Yield to clients: if a client took focus while our self-UI is open
			// (its hotkey/combo, or the launcher picking it), close the self-UI
			// instead of re-grabbing focus below. Skip during combo recording
			// (it needs the UI up). Fires once, so the save is a one-shot.
			if (SettingsUI::IsVisible() && !ComboRecording::IsActive() &&
				m_focused_client != 0 && m_focused_client != m_self_client_id) {
				SettingsUI::SetVisible(false);
				Overlay::SaveSettings();
				logs::info("Client {} took focus; self-UI auto-closed (yielded)", m_focused_client);
			}

			// Only the visible settings UI claims self-focus. The rebind capture
			// renders as an on-top overlay over the focused client, so focus stays
			// with whoever (a client, or the helper menu) started the rebind.
			const bool selfActive = SettingsUI::IsVisible();
			if (selfActive) {
				if (m_focused_client != m_self_client_id) {
					RequestFocus(m_self_client_id);
				}
			} else if (m_focused_client == m_self_client_id) {
				ReleaseFocus(m_self_client_id);
				// Persist on every close — single SaveSettings site so no close
				// path can miss it, and edits survive a crash before a graceful exit.
				Overlay::SaveSettings();
				logs::info("Self-UI close detected; focus released, settings persisted");
			}
		}

		// Latch rising edges; held-and-already-matched combos do not re-fire.
		std::scoped_lock lk{ m_mutex };
		for (auto& [id, combo] : m_combos) {
			const bool matched = MatchCombo(combo.keys);
			if (matched && !combo.was_matched) {
				combo.latched = true;
			}
			combo.was_matched = matched;
		}
	}

	std::vector<std::pair<uint32_t, std::string>> HelperImpl::BuildOverlayOrder()
	{
		// Helper's own UI first, then every non-HUD client (HUD overlays are
		// always-on, not switchable). Clients follow the user's saved order
		// (Settings::overlayOrder, by name); unlisted clients fall to the end in
		// registration-id order. Backs cycling, the toast counter, and the open
		// combo's first-mod pick.
		std::vector<std::pair<uint32_t, std::string>> order;
		std::scoped_lock lk{ m_mutex };
		if (m_self_client_id != 0)
			order.emplace_back(m_self_client_id, "ImGuiVRHelper");

		std::vector<std::pair<uint32_t, std::string>> clients;
		for (const auto& [id, rec] : m_clients) {
			if (id == m_self_client_id)
				continue;
			if (rec.flags & ImGuiVRHelperPluginAPI::kClientFlag_HUDMode)
				continue;
			// PointerFocus clients are coexistence overlays the owning mod drives itself via
			// RequestFocus (e.g. an in-conversation panel that shares the wand with the game
			// menu) — not user-pickable menus. Keep them out of the cycle / quick-select / the
			// cold-start "first overlay" pick, so cycling can't land on an auto-only panel.
			if (rec.flags & ImGuiVRHelperPluginAPI::kClientFlag_PointerFocus)
				continue;
			clients.emplace_back(id, rec.name);
		}

		const auto& pref = Overlay::State::GetSingleton().settings.overlayOrder;
		auto rank = [&pref](const std::string& name) -> std::size_t {
			const auto it = std::find(pref.begin(), pref.end(), name);
			return it == pref.end() ? pref.size() : static_cast<std::size_t>(it - pref.begin());
		};
		std::sort(clients.begin(), clients.end(), [&](const auto& a, const auto& b) {
			const std::size_t ra = rank(a.second);
			const std::size_t rb = rank(b.second);
			if (ra != rb)
				return ra < rb;
			return a.first < b.first;  // stable fallback: registration id
		});
		for (auto& c : clients)
			order.push_back(std::move(c));
		return order;
	}

	void HelperImpl::CycleOverlay(int direction)
	{
		const auto order = BuildOverlayOrder();
		if (order.empty())
			return;
		const uint32_t current = m_focused_client;  // racy read is fine (uint)

		int idx = 0;
		for (size_t i = 0; i < order.size(); ++i) {
			if (order[i].first == current) {
				idx = static_cast<int>(i);
				break;
			}
		}
		const int n = static_cast<int>(order.size());
		idx = ((idx + direction) % n + n) % n;  // wrap in both directions
		const uint32_t target = order[idx].first;

		if (target == m_self_client_id) {
			SettingsUI::SetVisible(true);
			RequestFocus(m_self_client_id);
		} else {
			// Hand focus to the client and hide our UI so the reconciler doesn't
			// reclaim focus — same yield path as the launcher dropdown.
			SettingsUI::SetVisible(false);
			RequestFocus(target);
		}
		logs::info("CycleOverlay({}) -> client {}", direction, target);
	}

	void HelperImpl::ProcessOverlayCycleInput(float dt)
	{
		auto& state = Overlay::State::GetSingleton();
		using K = RE::BSOpenVRControllerDevice::Keys;

		const bool leftHanded = state.lastKnownLeftHandedMode;
		const auto& leftCtl = leftHanded ? state.primaryControllerState : state.secondaryControllerState;
		const auto& rightCtl = leftHanded ? state.secondaryControllerState : state.primaryControllerState;
		const bool leftClick = leftCtl[K::kJoystickTrigger].isPressed;
		const bool rightClick = rightCtl[K::kJoystickTrigger].isPressed;
		const bool leftTrigger = leftCtl[K::kTrigger].isPressed;
		const bool rightTrigger = rightCtl[K::kTrigger].isPressed;
		const bool leftGrip = leftCtl[K::kGrip].isPressed || leftCtl[K::kGripAlt].isPressed;
		const bool rightGrip = rightCtl[K::kGrip].isPressed || rightCtl[K::kGripAlt].isPressed;

		const size_t leftThumb = leftHanded ? static_cast<size_t>(RE::ControllerRole::Primary) : static_cast<size_t>(RE::ControllerRole::Secondary);
		const size_t rightThumb = leftHanded ? static_cast<size_t>(RE::ControllerRole::Secondary) : static_cast<size_t>(RE::ControllerRole::Primary);

		// Cycle or quick-select only when an overlay is open and the wand is off-panel.
		if (m_focused_client != 0 && !state.wandState.isIntersecting) {
			// Update click durations
			if (leftClick) {
				m_leftStickClickDuration += dt;
			} else {
				if (m_leftStickClickDuration > 0.0f && m_leftStickClickDuration < 0.5f) {
					if (!m_quickSelectActive)
						CycleOverlay(-1);
				}
				m_leftStickClickDuration = 0.0f;
			}

			if (rightClick) {
				m_rightStickClickDuration += dt;
			} else {
				if (m_rightStickClickDuration > 0.0f && m_rightStickClickDuration < 0.5f) {
					if (!m_quickSelectActive)
						CycleOverlay(+1);
				}
				m_rightStickClickDuration = 0.0f;
			}

			// Edge-detect hold transitions (>= 0.5 seconds)
			if (!m_quickSelectActive) {
				if (m_leftStickClickDuration >= 0.5f) {
					m_quickSelectActive = true;
					m_quickSelectLeftHand = true;
					m_quickSelectScrollTimer = 0.0f;
					m_quickSelectStickReleased = false;
					m_quickSelectOriginalFocus = m_focused_client;

					// Set initial hovered client to current focused client
					const auto order = BuildOverlayOrder();
					m_quickSelectHoveredIdx = 0;
					for (size_t i = 0; i < order.size(); ++i) {
						if (order[i].first == m_focused_client) {
							m_quickSelectHoveredIdx = static_cast<int>(i);
							break;
						}
					}

					// Force helper UI focus
					SettingsUI::SetVisible(true);
					RequestFocus(m_self_client_id);
				} else if (m_rightStickClickDuration >= 0.5f) {
					m_quickSelectActive = true;
					m_quickSelectLeftHand = false;
					m_quickSelectScrollTimer = 0.0f;
					m_quickSelectStickReleased = false;
					m_quickSelectOriginalFocus = m_focused_client;

					// Set initial hovered client to current focused client
					const auto order = BuildOverlayOrder();
					m_quickSelectHoveredIdx = 0;
					for (size_t i = 0; i < order.size(); ++i) {
						if (order[i].first == m_focused_client) {
							m_quickSelectHoveredIdx = static_cast<int>(i);
							break;
						}
					}

					// Force helper UI focus
					SettingsUI::SetVisible(true);
					RequestFocus(m_self_client_id);
				}
			}
		} else {
			// If wand starts intersecting or overlay closed, cancel quick select
			if (m_quickSelectActive) {
				m_quickSelectActive = false;
				m_leftStickClickDuration = 0.0f;
				m_rightStickClickDuration = 0.0f;
				SettingsUI::SetVisible(false);
				RequestFocus(m_self_client_id);  // Releases or goes to default
			}
			m_leftStickClickDuration = 0.0f;
			m_rightStickClickDuration = 0.0f;
		}

		// Handle quick select navigation & select/cancel
		if (m_quickSelectActive) {
			const bool isStillClicked = leftClick || rightClick;
			const bool isTriggerPressed = leftTrigger || rightTrigger;
			const bool isGripPressed = leftGrip || rightGrip;

			// Track when the user has released the initial long-hold click
			if (!isStillClicked) {
				m_quickSelectStickReleased = true;
			}

			if (isGripPressed) {
				// Cancel and restore original focus
				m_quickSelectActive = false;
				m_leftStickClickDuration = 0.0f;
				m_rightStickClickDuration = 0.0f;
				if (m_quickSelectOriginalFocus == m_self_client_id) {
					SettingsUI::SetVisible(true);
					RequestFocus(m_self_client_id);
				} else {
					SettingsUI::SetVisible(false);
					RequestFocus(m_quickSelectOriginalFocus);
				}
			} else if (isTriggerPressed || (m_quickSelectStickReleased && (leftClick || rightClick))) {
				// Select hovered overlay
				m_quickSelectActive = false;
				m_leftStickClickDuration = 0.0f;
				m_rightStickClickDuration = 0.0f;
				const auto order = BuildOverlayOrder();
				if (!order.empty() && m_quickSelectHoveredIdx >= 0 && m_quickSelectHoveredIdx < static_cast<int>(order.size())) {
					const uint32_t target = order[m_quickSelectHoveredIdx].first;
					if (target == m_self_client_id) {
						SettingsUI::SetVisible(true);
						RequestFocus(m_self_client_id);
					} else {
						SettingsUI::SetVisible(false);
						RequestFocus(target);
					}
				}
			} else {
				// Process stick Y movement for navigation from either hand
				const float leftSy = leftCtl.thumbsticks[leftThumb].y;
				const float rightSy = rightCtl.thumbsticks[rightThumb].y;
				const float sy = (std::abs(leftSy) >= std::abs(rightSy)) ? leftSy : rightSy;

				m_quickSelectScrollTimer -= dt;

				if (m_quickSelectScrollTimer <= 0.0f) {
					const auto order = BuildOverlayOrder();
					const int n = static_cast<int>(order.size());
					if (n > 0) {
						// Note: stick up (sy > 0.4f) goes up in the list (idx - 1),
						// stick down (sy < -0.4f) goes down in the list (idx + 1).
						if (sy > 0.4f) {
							m_quickSelectHoveredIdx = (m_quickSelectHoveredIdx - 1 + n) % n;
							m_quickSelectScrollTimer = 0.25f;  // rate limit scrolling to 250ms
						} else if (sy < -0.4f) {
							m_quickSelectHoveredIdx = (m_quickSelectHoveredIdx + 1) % n;
							m_quickSelectScrollTimer = 0.25f;
						}
					}
				}
			}
		}

		m_prevLeftStickClick = leftClick;
		m_prevRightStickClick = rightClick;
	}

	// Snapshot clients under the lock, render the self UI, then run each
	// client's on_frame outside the lock (a client may call back into
	// Register/UnregisterClient). Returns the focused id captured this frame.
	uint32_t HelperImpl::DispatchToClients(const ImGuiVRHelperPluginAPI::Frame& baseFrame, float dt)
	{
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

		const bool wandHit = Overlay::State::GetSingleton().wandState.isIntersecting;

		// Render the helper's own settings UI into its self-client panel before
		// the client loop so the eye composite picks up the latest pixels.
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

		// While capturing a rebind, the overlay is modal: swallow controller input
		// from clients so the focused client doesn't act on the combo the user is
		// pressing. Keep masking after capture ends until every button releases —
		// otherwise keys still held from the capture leak in as menu clicks.
		const bool recording = ComboRecording::IsActive();
		if (m_prevRecording && !recording)
			m_inputSettling = true;
		m_prevRecording = recording;
		if (m_inputSettling &&
			(baseFrame.left.buttons_held | baseFrame.right.buttons_held) == 0)
			m_inputSettling = false;
		const bool maskInput = recording || m_inputSettling;

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

			if (maskInput) {
				for (auto* h : { &perClient.left, &perClient.right }) {
					h->buttons_held = h->buttons_pressed = h->buttons_released = h->buttons_touched = 0;
					h->trigger = h->grip = 0.0f;
					h->stick_x = h->stick_y = h->pad_x = h->pad_y = 0.0f;
				}
			}

			if (sn.on_frame) {
				sn.on_frame(&perClient, sn.user);
			}
		}

		return focused;
	}

	// End-of-frame post-processing: drag-highlight tint, HUD-demo smoke test,
	// overlay-visible sync. Texture submission to the headset
	// happens later, lazily, from the IVRCompositor::Submit detour.
	void HelperImpl::PostProcessFrame(uint32_t focused, float dt)
	{
		auto& overlayState = Overlay::State::GetSingleton();

		// Tint the focused panel (drag-highlight colour while dragging) into the
		// post-process texture InSceneOverlay samples. Skip when nothing's focused.
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

		// HUD-mode smoke test. On the demo's on→off edge, clear the RTV once so
		// the last frame's pixels stop being composited; while off-and-was-off,
		// do nothing.
		static bool s_prevDemoOn = false;
		const bool demoOn = overlayState.settings.showHUDDemo;
		if (m_hud_demo_client_id != 0 && (demoOn || s_prevDemoOn)) {
			ImGuiVRHelperPluginAPI::PanelHandle handle{};
			if (GetPanel(m_hud_demo_client_id, &handle) && handle.rtv) {
				if (demoOn) {
					HUDDemo::Render(handle.rtv, handle.width, handle.height);
				} else {
					ClearRtvTransparent(handle.rtv);
				}
			}
		}
		s_prevDemoOn = demoOn;

		// Swap toast: when the focused overlay changes, flash its name for a
		// couple of seconds so the user knows what they switched to. Rendered as
		// a HUD-mode layer over whatever is now focused.
		constexpr float kToastDurationSeconds = 2.0f;
		constexpr float kToastFadeSeconds = 0.6f;  // fade over the final stretch
		if (focused != m_lastFocusForToast) {
			m_lastFocusForToast = focused;
			if (focused != 0) {
				// Name + position in the cycle (e.g. "CommunityShaders  (1/2)")
				// so the user knows where they are in the overlay list.
				const auto order = BuildOverlayOrder();
				for (size_t i = 0; i < order.size(); ++i) {
					if (order[i].first == focused) {
						m_toastText = order[i].second + "  (" + std::to_string(i + 1) +
						              "/" + std::to_string(order.size()) + ")";
						m_toastRemaining = kToastDurationSeconds;
						break;
					}
				}
			}
		}
		if (m_toast_client_id != 0 && m_toastRemaining > 0.0f) {
			m_toastRemaining -= dt;
			const bool active = m_toastRemaining > 0.0f;
			ImGuiVRHelperPluginAPI::PanelHandle handle{};
			if (GetPanel(m_toast_client_id, &handle) && handle.rtv) {
				if (active) {
					const float alpha = m_toastRemaining >= kToastFadeSeconds ?
					                        1.0f :
					                        m_toastRemaining / kToastFadeSeconds;
					ToastHUD::Render(handle.rtv, m_toastText, alpha);
				} else {
					ClearRtvTransparent(handle.rtv);  // expired → clear once
				}
			}
		}

		// Startup welcome banner. Shows once from the first rendered frame until
		// it times out, the user enters the world, or the setting is off. Reuses
		// the toast HUD renderer, into its own always-on HUD-mode panel.
		constexpr float kWelcomeDurationSeconds = 30.0f;
		constexpr float kWelcomeFadeSeconds = 1.0f;
		if (!overlayState.settings.showWelcome || m_enteredGame)
			m_welcomeDismissed = true;
		if (!m_welcomeDismissed) {
			if (!m_welcomeStarted) {
				m_welcomeStarted = true;
				m_welcomeRemaining = kWelcomeDurationSeconds;
			}
			m_welcomeRemaining -= dt;
			if (m_welcomeRemaining <= 0.0f)
				m_welcomeDismissed = true;
		}
		if (m_welcome_client_id != 0) {
			const bool show = m_welcomeStarted && !m_welcomeDismissed;
			if (show || m_welcomeWasShown) {  // render while up; clear once on dismiss
				ImGuiVRHelperPluginAPI::PanelHandle handle{};
				if (GetPanel(m_welcome_client_id, &handle) && handle.rtv) {
					if (show) {
						const float alpha = m_welcomeRemaining >= kWelcomeFadeSeconds ?
						                        1.0f :
						                        m_welcomeRemaining / kWelcomeFadeSeconds;
						// Color-coded per-controller open combo so the banner shows
						// exactly how to open the menu (like CS's welcome did), plus
						// the pause-to-open hint when that gate is on.
						ToastHUD::RenderWelcome(handle.rtv, alpha, overlayState.settings.openMenuKeys,
							overlayState.settings.onlyOpenWhilePaused);
					} else {
						ClearRtvTransparent(handle.rtv);
					}
				}
			}
			m_welcomeWasShown = show;
		}

		// Combo-rebind capture overlay. Render into its panel while a recording
		// is active so InSceneOverlay can composite it on top of the focused
		// client; clear once on the active→inactive edge so it stops drawing.
		if (m_rebind_client_id != 0) {
			const bool active = ComboRecording::IsActive();
			if (active || m_rebindWasActive) {
				ImGuiVRHelperPluginAPI::PanelHandle handle{};
				if (GetPanel(m_rebind_client_id, &handle) && handle.rtv) {
					if (active) {
						ComboRecording::RenderToPanel(handle.rtv);
					} else {
						ClearRtvTransparent(handle.rtv);
					}
				}
			}
			m_rebindWasActive = active;
		}

		// Keep overlayVisible in sync so OverlayDrag::CanPerform sees it next frame.
		overlayState.overlayVisible = (focused != 0);
	}

	void HelperImpl::DispatchFrame(float dt)
	{
		ZoneScopedN("Helper::DispatchFrame");
		++m_frameCounter;  // HUD-idle skipping compares GetPanel access against this
		UpdateWandPointer();

		ImGuiVRHelperPluginAPI::Frame baseFrame;
		Input::BuildFrame(baseFrame, dt);
		baseFrame.hud_depth = std::max(0.3f, Overlay::State::GetSingleton().settings.hudDepth);
		baseFrame.hud_coverage = std::clamp(Overlay::State::GetSingleton().settings.hudCoverage,
			Overlay::Config::kMinHUDCoverage, Overlay::Config::kMaxHUDCoverage);

		// Drag state machine + combo recording before the focus reconciler,
		// which treats ComboRecording::IsActive() as part of "self active".
		OverlayDrag::UpdateFixedWorldPositioning();
		OverlayDrag::Update();
		ComboRecording::Tick(dt);

		ReconcileSelfFocusAndCombos();
		ProcessOverlayCycleInput(dt);

		const uint32_t focused = DispatchToClients(baseFrame, dt);

		PostProcessFrame(focused, dt);
	}

	bool HelperImpl::IsDashboardVisible()
	{
		return false;
	}

	void HelperImpl::SetKeyboardActive(uint32_t client_id, bool active, const char* seed_utf8)
	{
		VRKeyboard::SetActive(client_id, active, seed_utf8);
	}

	uint32_t HelperImpl::GetKeyboardText(uint32_t client_id, char* out_utf8, uint32_t out_size)
	{
		return VRKeyboard::GetText(client_id, out_utf8, out_size);
	}

	bool HelperImpl::ConsumeKeyboardClosed(uint32_t client_id)
	{
		return VRKeyboard::ConsumeClosed(client_id);
	}
}
