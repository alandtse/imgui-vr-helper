// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Concrete implementation of ImGuiVRHelperPluginAPI::IImGuiVRHelperInterface001.
//
// All methods are stubs in the initial scaffold. Real implementations land
// in subsequent PRs, lifted from
// skyrim-community-shaders/src/Features/VR/{Input,InSceneOverlay,
// OverlayDrag,WandPointing,OpenVRDetection}.cpp.

#pragma once

#include "ImGuiVRHelperAPI.h"

namespace ImGuiVRHelper
{
	class HelperImpl final : public ImGuiVRHelperPluginAPI::IImGuiVRHelperInterface001
	{
	public:
		static HelperImpl& GetSingleton();

		// IImGuiVRHelperInterface001
		uint32_t GetBuildNumber() override;

		uint32_t RegisterClient(const char* name, const char* version,
			ImGuiVRHelperPluginAPI::OnFrameFn on_frame, void* user, uint32_t flags) override;
		void UnregisterClient(uint32_t client_id) override;

		bool GetPanel(uint32_t client_id, ImGuiVRHelperPluginAPI::PanelHandle* out) override;
		bool GetPointer(uint32_t client_id, float* u, float* v, uint32_t* device_idx) override;

		ImGuiVRHelperPluginAPI::ComboId RegisterCombo(uint32_t client_id,
			const ImGuiVRHelperPluginAPI::InputCombo* keys, std::size_t n,
			float timeout_s) override;
		bool ComboFired(ImGuiVRHelperPluginAPI::ComboId) override;
		void StartComboRecording(uint32_t client_id, const char* label,
			ImGuiVRHelperPluginAPI::ComboRecordedFn on_done, void* user,
			float timeout_s) override;
		void CancelComboRecording(uint32_t client_id) override;

		bool IsOverlayVisible() override;
		void RequestFocus(uint32_t client_id) override;
		void ReleaseFocus(uint32_t client_id) override;

		void TriggerHaptic(uint32_t client_id, uint32_t haptic_token,
			uint32_t duration_us, float frequency, float amplitude) override;

		bool ImportLegacySettings(const char* json_blob) override;

		void FeedVREvent(uint32_t device, uint32_t key_code, bool pressed,
			float thumbstick_x, float thumbstick_y) override;

		bool IsDashboardVisible() override;

		// Helper-internal entry points (not part of the public API).

		/// Build the per-frame Frame snapshot via Input::BuildFrame and
		/// invoke each registered client's on_frame callback. Called from
		/// the helper's Present detour. `dt` is seconds since previous call.
		void DispatchFrame(float dt);

		/// Returns the raw ID3D11Texture2D backing a client's panel, or
		/// nullptr if the client doesn't exist or its texture isn't
		/// allocated yet. Used by InSceneOverlay to feed the eye-render
		/// compositing pass.
		ID3D11Texture2D* GetClientPanelTexture(uint32_t client_id);

		/// Snapshot of all currently-registered HUD-mode clients
		/// (kClientFlag_HUDMode set in their flags), returned as
		/// (client_id, panel_texture) pairs. Both fields filled — the
		/// texture is the result of EnsureClientTextureLocked, so it's
		/// either a valid ID3D11Texture2D* or nullptr if D3D isn't
		/// ready / texture creation failed. InSceneOverlay::RenderForEye
		/// iterates this list and composites each as a full-viewport
		/// alpha-blended quad before drawing the focused panel-mode
		/// client. Iteration order matches registration order so HUD
		/// layers stack predictably.
		struct HUDClientSnapshot
		{
			uint32_t client_id;
			ID3D11Texture2D* texture;
		};
		std::vector<HUDClientSnapshot> SnapshotHUDClients();

		/// Snapshot of every registered client for diagnostic display.
		/// Used by the helper's settings UI to show a 'Registered
		/// Clients' section. Read-only — modifying a client's flags
		/// or focus state via this snapshot does nothing.
		struct ClientSnapshot
		{
			uint32_t client_id;
			std::string name;
			std::string version;  ///< client-supplied at registration; empty if not provided
			uint32_t flags;
			bool has_texture;         ///< texture was allocated (client has called GetPanel)
			bool has_focus;           ///< this client currently holds focus
			bool dashboard_eligible;  ///< client has kClientFlag_Dashboard (appears in dashboard picker)
			bool dashboard_active;    ///< picker currently has this client selected (shown in dashboard rn)
		};
		std::vector<ClientSnapshot> SnapshotClients();

		/// Returns the currently-focused client_id, or 0 if no client
		/// holds focus. Used by InSceneOverlay to pick which client's
		/// panel to draw on each Submit.
		uint32_t GetFocusedClientId();

		/// Allocate the helper's own self-client (so the helper's settings
		/// UI gets a panel texture via the same per-client allocation
		/// pipeline as external clients). Idempotent.
		void EnsureSelfClient();

		/// True iff the helper's own settings UI is currently visible.
		bool IsSelfUIVisible();

		/// client_id of the helper's synthetic self-client, or 0 before
		/// EnsureSelfClient has run.
		uint32_t GetSelfClientId() const;

		/// Replace the self-toggle combo with `keys` (length `n`). Used
		/// by SettingsUI's "Rebind toggle" button after combo recording
		/// completes. No-op if `n` == 0.
		void RebindSelfToggle(const ImGuiVRHelperPluginAPI::InputCombo* keys, std::size_t n);

		/// Called from the PollInputDevices thunk on a keyboard-toggle
		/// key-down. Equivalent to firing the controller toggle combo —
		/// flips the helper's settings-UI visibility and adjusts focus.
		/// Exists because the main menu can swallow controller buttons
		/// before they reach our combo matcher; keyboard input always
		/// reaches us.
		void OnKeyboardToggle();

		/// True iff VR controller input should be intercepted from the
		/// game this frame — i.e. some helper UI is up and consuming
		/// trigger / grip / stick events. The PollInputDevices thunk
		/// swallows controller events when this returns true so menu
		/// clicks don't also fire bows / scroll thumbsticks don't move
		/// the player camera. Mirrors SCS Menu::ShouldSwallowInput.
		bool ShouldSwallowInput() const;

	private:
		HelperImpl() = default;

		// Dashboard subsystem reaches into m_clients / m_mutex / m_focused_client
		// directly to mirror panel textures onto SteamVR dashboard surfaces
		// each frame. Friend rather than passing accessors because the access
		// is intrinsic to the helper's lifecycle, not a public API extension.
		friend struct DashboardFriend;

		std::mutex m_mutex;
		uint32_t m_next_client_id = 1;
		std::unordered_map<uint32_t, struct ClientRecord> m_clients;
		std::unordered_map<ImGuiVRHelperPluginAPI::ComboId,
			struct ComboRecord>
			m_combos;
		ImGuiVRHelperPluginAPI::ComboId m_next_combo_id = 1;
		uint32_t m_focused_client = 0;

		/// Allocate (or return existing) per-client overlay texture
		/// resources. Returns true on success, false if D3D isn't ready
		/// or texture creation failed. Must be called with m_mutex held.
		bool EnsureClientTextureLocked(struct ClientRecord& rec);

		/// client_id assigned to the helper itself for its settings UI.
		/// 0 until EnsureSelfClient runs. Reserved combo IDs and the
		/// helper's panel texture are owned by this slot.
		uint32_t m_self_client_id = 0;

		/// Combo IDs the helper registers for its own toggle hotkey.
		ImGuiVRHelperPluginAPI::ComboId m_self_toggle_combo = 0;

		/// Synthetic HUD-mode client used by the Settings::showHUDDemo
		/// smoke test. Registered once during EnsureSelfClient. Each
		/// frame DispatchFrame checks the toggle and clears this
		/// client's panel RTV with a red wash so the user can verify
		/// the kClientFlag_HUDMode composite path end-to-end before
		/// any real client connects.
		uint32_t m_hud_demo_client_id = 0;
	};

	struct ClientRecord
	{
		std::string name;
		std::string version;  ///< client-supplied at registration; empty if not provided
		ImGuiVRHelperPluginAPI::OnFrameFn on_frame = nullptr;
		void* user = nullptr;
		uint32_t flags = 0;

		// Helper-owned panel render target. Allocated lazily on the first
		// GetPanel call after InitD3D has fired. Format is fixed at
		// R8G8B8A8_UNORM; dimensions match Overlay::Config::kOverlayWidth/
		// kOverlayHeight (1920×1080).
		winrt::com_ptr<ID3D11Texture2D> texture;
		winrt::com_ptr<ID3D11RenderTargetView> rtv;
		winrt::com_ptr<ID3D11ShaderResourceView> srv;

		// SteamVR Dashboard eligibility — purely informational on the
		// per-client side. The helper owns ONE shared dashboard overlay
		// (handle + thumbnail), and a picker inside the helper's
		// settings panel chooses which kClientFlag_Dashboard client's
		// panel texture is mirrored onto it. So we don't track per-
		// client overlay handles here; whether this client appears in
		// the picker is a function of (flags & kClientFlag_Dashboard).
		std::string dashboard_thumbnail_path;  ///< optional icon for the picker entry; not yet rendered (v1 picker is text-only)
	};

	struct ComboRecord
	{
		uint32_t client_id = 0;
		std::vector<ImGuiVRHelperPluginAPI::InputCombo> keys;
		float timeout_s = 0.0f;
		bool latched = false;      ///< edge-fired, cleared on read
		bool was_matched = false;  ///< previous-frame match, for rising edge detection
	};
}
