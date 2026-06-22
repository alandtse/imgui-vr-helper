// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Transient "now showing <overlay>" banner shown briefly when VR focus swaps
// between overlays, so the user knows which one they're looking at. Rendered in
// a dedicated, non-interactive ImGui context into a HUD-mode client's panel, so
// the existing HUD compositing pass draws it on top of whichever overlay is
// focused. Same pattern as HUDDemo.

#pragma once

#include <string>

struct ID3D11RenderTargetView;

namespace ImGuiVRHelper::ToastHUD
{
	/// Render the banner with `text` top-center into the HUD-mode panel `rtv` at
	/// `alpha` in [0,1] (for fade-out). Lazily creates the context + DX11 backend.
	/// No-op if `rtv` is null, `text` is empty, or D3D isn't ready.
	void Render(ID3D11RenderTargetView* rtv, const std::string& text, float alpha);

	/// Clear the panel to transparent (toast expired) so the HUD pass stops
	/// compositing the last frame's pixels.
	void ClearToTransparent(ID3D11RenderTargetView* rtv);

	/// Destroy the context + DX11 backend. Safe if never initialized.
	void Shutdown();
}
