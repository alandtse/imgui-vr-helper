// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Stub implementations. Each method logs and returns a default value. Real
// behavior arrives in subsequent PRs (overlay submission, controller
// polling, combo matching, drag, wand pointing, in-scene fallback).

#include "pch.h"

#include "Globals.h"
#include "HelperImpl.h"
#include "Input.h"
#include "Overlay.h"
#include "OverlayManager.h"
#include "WandPointing.h"
#include "internal/VRUtils.h"

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
		ImGuiVRHelperPluginAPI::ComboRecordedFn /*on_done*/, void* /*user*/,
		float timeout_s)
	{
		logs::info("StartComboRecording(client={}, label={}, timeout={}s) [stub]",
			client_id, label ? label : "<null>", timeout_s);
		// TODO: render modal capture overlay; latch input until done/timeout.
	}

	void HelperImpl::CancelComboRecording(uint32_t client_id)
	{
		logs::info("CancelComboRecording(client={}) [stub]", client_id);
	}

	bool HelperImpl::IsOverlayVisible()
	{
		return OverlayManager::IsVisible();
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

	void HelperImpl::TriggerHaptic(uint32_t /*client_id*/, uint32_t /*haptic_token*/,
		uint32_t /*duration_us*/, float /*frequency*/, float /*amplitude*/)
	{
		// TODO: route to OpenVR IVRSystem::TriggerHapticPulse.
	}

	bool HelperImpl::ImportLegacySettings(const char* /*json_blob*/)
	{
		// TODO: merge into helper's persistent JSON.
		return true;
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

		// After all clients have rendered into their panel textures,
		// submit the focused client's texture to the IVROverlay handle.
		OverlayManager::SubmitFrame(focused);
	}
}
