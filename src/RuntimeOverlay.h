// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#pragma once

#include <openvr.h>

namespace ImGuiVRHelper::RuntimeOverlay
{
	/// Present-thread observer. Records the Present producer identity and masks
	/// hosting if the submit tick stops; never prepares work or calls the runtime.
	void RenderTick();

	/// The single graphics owner: called from the IVRCompositor::Submit detour on
	/// the thread that owns the game's D3D11 immediate context. Owns preparation,
	/// every IVROverlay call and cleanup; ticks from other threads are refused.
	void SubmitTick(vr::EVREye eye);

	/// Requests a fault from any thread; the owner consumes it on its next tick.
	void ReportFault(const char* reason);

	/// True while the runtime accepted the panel calls and ownership is intact.
	bool IsHostingPanel();

	/// One-line reason for the current hosting state, for the settings UI.
	const char* StatusText();
}
