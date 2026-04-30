// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Stub implementations. Each method logs and returns a default value. Real
// behavior arrives in subsequent PRs (overlay submission, controller
// polling, combo matching, drag, wand pointing, in-scene fallback).

#include "pch.h"

#include "HelperImpl.h"

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

	bool HelperImpl::GetPanel(uint32_t /*client_id*/,
		ImGuiVRHelperPluginAPI::PanelHandle* out)
	{
		if (out) {
			out->width = 0;
			out->height = 0;
			out->rtv = nullptr;
		}
		// TODO: return helper-owned RTV once Overlay.{h,cpp} lands.
		return false;
	}

	bool HelperImpl::GetPointer(uint32_t /*client_id*/, float* u, float* v,
		uint32_t* device_idx)
	{
		if (u)
			*u = 0.0f;
		if (v)
			*v = 0.0f;
		if (device_idx)
			*device_idx = 0;
		// TODO: return wand intersection once WandPointing.cpp lands.
		return false;
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
		return false;
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
}
