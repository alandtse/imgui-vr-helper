// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include "pch.h"

#include "OverlayManager.h"

#include "HelperImpl.h"
#include "Overlay.h"
#include "internal/VRUtils.h"

#include <openvr.h>

namespace ImGuiVRHelper::OverlayManager
{
	namespace
	{
		vr::VROverlayHandle_t g_overlayHandle = vr::k_ulOverlayHandleInvalid;
		bool g_visible = false;

		constexpr const char* kOverlayKey = "imgui-vr-helper.menu";
		constexpr const char* kOverlayName = "ImGuiVRHelper Menu";

		vr::IVROverlay* GetOverlay()
		{
			Util::OpenVRContext ctx;
			return ctx.HasOverlay() ? ctx.overlay : nullptr;
		}
	}

	void Init()
	{
		if (g_overlayHandle != vr::k_ulOverlayHandleInvalid) {
			return;  // already initialized
		}

		auto* overlay = GetOverlay();
		if (!overlay) {
			logs::warn("OverlayManager::Init: IVROverlay interface unavailable");
			return;
		}

		auto err = overlay->CreateOverlay(kOverlayKey, kOverlayName, &g_overlayHandle);
		if (err != vr::VROverlayError_None) {
			logs::warn("OverlayManager::Init: CreateOverlay failed (err={})",
				static_cast<int>(err));
			g_overlayHandle = vr::k_ulOverlayHandleInvalid;
			return;
		}

		// Sensible defaults for a settings-style panel.
		const auto& s = Overlay::State::GetSingleton().settings;
		overlay->SetOverlayWidthInMeters(g_overlayHandle, s.menuScale);
		overlay->SetOverlayInputMethod(g_overlayHandle, vr::VROverlayInputMethod_Mouse);
		overlay->SetOverlayFlag(g_overlayHandle, vr::VROverlayFlags_VisibleInDashboard, false);
		overlay->SetOverlayAlpha(g_overlayHandle, 1.0f);

		logs::info("OverlayManager::Init: overlay handle={} ({}m wide)",
			g_overlayHandle, s.menuScale);
	}

	void SubmitFrame(uint32_t focused_client)
	{
		if (g_overlayHandle == vr::k_ulOverlayHandleInvalid)
			return;

		auto* overlay = GetOverlay();
		if (!overlay)
			return;

		// Pick the texture to submit. For now, only the focused client's
		// panel is shown. With no focused client, hide the overlay.
		ID3D11Texture2D* tex = HelperImpl::GetSingleton().GetClientPanelTexture(focused_client);
		if (!tex) {
			if (g_visible) {
				overlay->HideOverlay(g_overlayHandle);
				g_visible = false;
				Overlay::State::GetSingleton().overlayVisible = false;
			}
			return;
		}

		// Position at the configured HMD-relative offset.
		const auto& s = Overlay::State::GetSingleton().settings;
		auto transform = Util::ComputeOverlayTransformFromHMD(
			s.hmdOffsetX, s.hmdOffsetY, s.hmdOffsetZ);
		overlay->SetOverlayTransformAbsolute(g_overlayHandle,
			vr::TrackingUniverseStanding, &transform);
		overlay->SetOverlayWidthInMeters(g_overlayHandle, s.menuScale);

		vr::Texture_t vrTex{};
		vrTex.handle = tex;
		vrTex.eType = vr::TextureType_DirectX;
		vrTex.eColorSpace = vr::ColorSpace_Auto;
		auto err = overlay->SetOverlayTexture(g_overlayHandle, &vrTex);
		if (err != vr::VROverlayError_None) {
			logs::warn("OverlayManager::SubmitFrame: SetOverlayTexture failed (err={})",
				static_cast<int>(err));
			return;
		}

		if (!g_visible) {
			overlay->ShowOverlay(g_overlayHandle);
			g_visible = true;
			Overlay::State::GetSingleton().overlayVisible = true;
		}
	}

	void Shutdown()
	{
		if (g_overlayHandle == vr::k_ulOverlayHandleInvalid)
			return;

		auto* overlay = GetOverlay();
		if (overlay) {
			overlay->DestroyOverlay(g_overlayHandle);
		}
		g_overlayHandle = vr::k_ulOverlayHandleInvalid;
		g_visible = false;
		Overlay::State::GetSingleton().overlayVisible = false;
	}

	bool IsVisible()
	{
		return g_visible;
	}
}
