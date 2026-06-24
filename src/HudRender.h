// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#pragma once

#include "Globals.h"

#include <d3d11.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>

namespace ImGuiVRHelper
{
	// Clears a helper-owned panel RTV to transparent — the "nothing to show this
	// frame" state every HUD layer shares.
	inline void ClearRtvTransparent(ID3D11RenderTargetView* rtv)
	{
		if (!rtv || !Globals::IsReady())
			return;
		const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		Globals::GetD3D().context->ClearRenderTargetView(rtv, clear);
	}

	// Composites the current ImGui frame into a helper-owned panel RTV. Call after
	// ImGui::Render(). Saves and restores the previously bound render target so the
	// surrounding compositing chain is undisturbed, and releases the COM refs
	// OMGetRenderTargets hands back — the leak-prone step that must not drift.
	inline void RenderDrawDataToRtv(ID3D11RenderTargetView* rtv)
	{
		auto* ctx = Globals::GetD3D().context;
		ID3D11RenderTargetView* oldRTV = nullptr;
		ID3D11DepthStencilView* oldDSV = nullptr;
		ctx->OMGetRenderTargets(1, &oldRTV, &oldDSV);

		const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		ctx->OMSetRenderTargets(1, &rtv, nullptr);
		ctx->ClearRenderTargetView(rtv, clear);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

		ctx->OMSetRenderTargets(1, &oldRTV, oldDSV);
		if (oldRTV)
			oldRTV->Release();
		if (oldDSV)
			oldDSV->Release();
	}
}
