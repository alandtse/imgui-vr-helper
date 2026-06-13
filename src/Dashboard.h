// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// SteamVR Dashboard overlay support (single-handle model).
//
// The helper owns ONE dashboard overlay + thumbnail handle, registered
// under "ImGuiVRHelper" in SteamVR's dashboard rail. A picker inside
// the helper's settings panel chooses which kClientFlag_Dashboard
// client's panel texture is currently mirrored onto the dashboard
// surface. Default is the helper's own self-client.
//
// Rationale: per-client dashboard handles produce a cluttered SteamVR
// rail when many clients are registered. One icon + a picker is
// cleaner UX, and the single-handle model means one CreateDashboardOverlay
// call regardless of client count.

#pragma once

#include <cstdint>

namespace ImGuiVRHelper
{
	namespace Dashboard
	{
		/// Idempotent per-frame entry point. Mirrors the active client's
		/// panel texture onto the shared dashboard overlay and pumps
		/// VREvent_* into ImGui input state.
		///
		/// No-op if IVROverlay isn't available (OpenComposite-style
		/// runtimes log once on first call and silently skip after).
		///
		/// Called from HelperImpl::DispatchFrame after per-client
		/// on_frame callbacks.
		void Tick();

		/// Tear down the shared dashboard handle. Called once at helper
		/// shutdown (currently just logged — process exit reaps the
		/// handle).
		void Shutdown();

		/// True iff the SteamVR dashboard is currently open. Cheap —
		/// reads cached state updated each Tick.
		bool IsDashboardVisible();

		/// True iff the panel the dashboard is currently mirroring resolved
		/// to the helper's own self-client (default selection, or a fallback
		/// for an unregistered / non-honoring active client). HelperImpl
		/// uses this to force-render the settings UI so the rail entry shows
		/// a live menu. Updated each Tick.
		bool IsShowingSelf();

		/// Switch the active dashboard client. Panel texture mirroring
		/// flips on the next Tick. `client_id` must be a registered
		/// client with kClientFlag_Dashboard set, or 0 to fall back to
		/// the helper's self-client. Silent no-op for invalid IDs.
		///
		/// Also calls RequestFocus on the new active client so its
		/// ImGui state receives input from the dashboard cursor.
		void SetActiveClient(uint32_t client_id);

		/// Returns the currently-active dashboard client, or 0 if no
		/// dashboard is allocated (runtime unavailable or shutdown).
		uint32_t GetActiveClient();

		/// Lock-free clear of the active client. Safe to call from
		/// HelperImpl methods that already hold m_mutex (the public
		/// SetActiveClient takes the lock to validate; this skips it).
		/// Used by UnregisterClient to drop a stale picker selection
		/// without nested-locking std::mutex.
		void ClearActiveClientIfMatches(uint32_t client_id);

		/// Update the dashboard's thumbnail icon from a PNG/JPG path.
		/// Loaded synchronously on the next Tick; absolute or
		/// game-root-relative path. Pass nullptr to clear (reverts to
		/// SteamVR's placeholder).
		void SetThumbnail(const char* image_path);
	}
}
