// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// SteamVR Dashboard overlay support.
//
// Owns the per-client `dashboard_overlay` / `dashboard_thumbnail` handles
// in ClientRecord. Each frame, mirrors the client's panel texture onto
// the dashboard plane via IVROverlay::SetOverlayTexture, drains
// VREvent_* events from each dashboard overlay, and translates dashboard
// mouse/keyboard events into ImGui input state for the focused client.
//
// The in-scene overlay path (InSceneOverlay.cpp) is independent — a
// client registered with both kClientFlag_Dashboard and as an in-scene
// panel renders on both surfaces from the same texture.

#pragma once

#include <cstdint>

namespace ImGuiVRHelper
{
	struct ClientRecord;

	namespace Dashboard
	{
		/// Idempotent per-frame entry point. Walks every registered
		/// client; for those with kClientFlag_Dashboard, ensures a
		/// dashboard overlay handle exists, mirrors the panel texture
		/// onto it, and drains VREvent_* into ImGui mouse/keyboard
		/// state for the focused client.
		///
		/// No-op if the OpenVR runtime hasn't initialised IVROverlay,
		/// or if no client has kClientFlag_Dashboard set.
		///
		/// Called from HelperImpl::DispatchFrame, after the per-client
		/// on_frame callbacks have run, so any newly-registered client
		/// is picked up the same frame it appears.
		void Tick();

		/// Allocate dashboard handles for `rec` if it has
		/// kClientFlag_Dashboard, IVROverlay is available, and we
		/// haven't already tried and failed.
		///
		/// `client_id` is used to derive a stable overlay key
		/// ("imgui-vr-helper.client.<id>") so SteamVR's dashboard
		/// position memory persists across sessions.
		///
		/// Caller must hold HelperImpl::m_mutex.
		bool EnsureClientLocked(uint32_t client_id, ClientRecord& rec);

		/// Tear down dashboard handles for a client about to be
		/// unregistered. Caller must hold HelperImpl::m_mutex.
		void ReleaseClientLocked(ClientRecord& rec);

		/// True iff the SteamVR dashboard is currently open. Cheap —
		/// reads a cached state updated each Tick.
		bool IsDashboardVisible();
	}
}
