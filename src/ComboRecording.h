// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Combo recording: implements the public StartComboRecording API. While
// recording is active, the helper presents a distinct "rebind" capture
// overlay — rendered in its own ImGui context into a dedicated panel and
// composited as a layer ON TOP of the focused client (so the client's menu
// stays put behind it, rather than swapping focus to the helper) — and
// accumulates the next set of pressed VR buttons into an InputCombo
// sequence. When the user releases all buttons, the callback fires with the
// captured combo. Timeout fires the callback with an empty result.

#pragma once

#include "ImGuiVRHelperAPI.h"

#include <cstddef>
#include <cstdint>

struct ID3D11RenderTargetView;

namespace ImGuiVRHelper::ComboRecording
{
	/// Begin recording for `client_id`. Cancels any in-progress recording
	/// for any client first (the modal is single-instance).
	void Begin(uint32_t client_id, const char* label,
		ImGuiVRHelperPluginAPI::ComboRecordedFn on_done, void* user, float timeout_s);

	/// Cancel a recording previously started by `client_id`. No-op if
	/// nothing is in progress for that client.
	void Cancel(uint32_t client_id);

	/// Returns true iff a recording is currently active.
	bool IsActive();

	/// client_id that started the active recording, or 0 if none.
	uint32_t ActiveClient();

	/// Per-frame tick. Reads Overlay::State controller state, detects
	/// newly-pressed buttons, accumulates them, and fires the callback
	/// when all buttons release after at least one was pressed (or on
	/// timeout). Call from DispatchFrame before client OnFrame loop.
	void Tick(float dt);

	/// Render the rebind capture overlay into `rtv` using a dedicated,
	/// non-interactive ImGui context (own font atlas + DX11 backend, like
	/// ToastHUD). The helper composites this panel on top of the focused
	/// client. No-op if not active. Call once per frame while IsActive().
	void RenderToPanel(ID3D11RenderTargetView* rtv);

	/// Clear `rtv` to transparent — call once on the active→inactive edge so
	/// the last captured frame stops compositing.
	void ClearToTransparent(ID3D11RenderTargetView* rtv);
}
