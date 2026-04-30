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

		uint32_t RegisterClient(const char* name, ImGuiVRHelperPluginAPI::OnFrameFn on_frame,
			void* user, uint32_t flags) override;
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

		// Helper-internal entry points (not part of the public API).

		/// Build the per-frame Frame snapshot via Input::BuildFrame and
		/// invoke each registered client's on_frame callback. Called from
		/// the helper's Present detour. `dt` is seconds since previous call.
		void DispatchFrame(float dt);

		/// Returns the raw ID3D11Texture2D backing a client's panel, or
		/// nullptr if the client doesn't exist or its texture isn't
		/// allocated yet. Used by OverlayManager to feed the IVROverlay.
		ID3D11Texture2D* GetClientPanelTexture(uint32_t client_id);

	private:
		HelperImpl() = default;

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
	};

	struct ClientRecord
	{
		std::string name;
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
