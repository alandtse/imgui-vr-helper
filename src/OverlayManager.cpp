// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include "pch.h"

#include "OverlayManager.h"

#include "Globals.h"
#include "HelperImpl.h"
#include "Overlay.h"
#include "OverlayTinter.h"
#include "internal/VRUtils.h"

#include <RE/B/BSOpenVR.h>

namespace ImGuiVRHelper::OverlayManager
{
	namespace
	{
		vr::VROverlayHandle_t g_handle = vr::k_ulOverlayHandleInvalid;

		// One-shot warning latches so we don't spam the log with the same
		// failure 90 times a second.
		bool g_warnedNoOverlay = false;
		bool g_warnedNoCleanOverlay = false;
		int g_setTextureErrLogCount = 0;

		void SetInputFlags(vr::IVROverlay* overlay, vr::VROverlayHandle_t handle)
		{
			if (!overlay || handle == vr::k_ulOverlayHandleInvalid)
				return;
			// Mirror SCS's flag set: scroll + touchpad + gamepad events route
			// to the overlay, and the overlay is visible in the SteamVR
			// dashboard for debugging.
			overlay->SetOverlayFlag(handle, vr::VROverlayFlags_SendVRScrollEvents, true);
			overlay->SetOverlayFlag(handle, vr::VROverlayFlags_SendVRTouchpadEvents, true);
			overlay->SetOverlayFlag(handle, vr::VROverlayFlags_AcceptsGamepadEvents, true);
			overlay->SetOverlayFlag(handle, vr::VROverlayFlags_VisibleInDashboard, true);
		}
	}

	bool EnsureInitialized()
	{
		if (g_handle != vr::k_ulOverlayHandleInvalid)
			return true;
		if (!Globals::IsReady())
			return false;

		Util::OpenVRContext ctx;
		if (!ctx.HasOverlay()) {
			if (!g_warnedNoOverlay) {
				logs::warn("OverlayManager::EnsureInitialized: BSOpenVR overlay not ready yet; will retry");
				g_warnedNoOverlay = true;
			}
			return false;
		}

		const std::string key = "imguivrhelper.menu";
		const std::string name = "ImGuiVRHelper Menu";
		const auto err = ctx.overlay->CreateOverlay(key.c_str(), name.c_str(), &g_handle);
		if (err != vr::VROverlayError_None) {
			logs::error("OverlayManager: CreateOverlay failed (err={})",
				static_cast<int>(err));
			g_handle = vr::k_ulOverlayHandleInvalid;
			return false;
		}

		SetInputFlags(ctx.overlay, g_handle);
		ctx.overlay->SetOverlayWidthInMeters(g_handle, 1.0f);
		ctx.overlay->SetOverlayAlpha(g_handle, 1.0f);

		logs::info("OverlayManager: created IVROverlay handle 0x{:X} (key='{}')",
			g_handle, key);
		return true;
	}

	void Tick()
	{
		if (!EnsureInitialized())
			return;

		Util::OpenVRContext ctx;
		if (!ctx.HasOverlay())
			return;

		// SteamVR refuses SetOverlayTexture on the game's proxied overlay,
		// so we keep a separate "clean" interface around for texture
		// submission only.
		auto* cleanOverlay = RE::BSOpenVR::GetCleanIVROverlay();
		if (!cleanOverlay && !g_warnedNoCleanOverlay) {
			logs::warn("OverlayManager: GetCleanIVROverlay returned nullptr; texture submit disabled");
			g_warnedNoCleanOverlay = true;
		}

		const auto& overlayState = Overlay::State::GetSingleton();
		const auto& s = overlayState.settings;
		const uint32_t focused = HelperImpl::GetSingleton().GetFocusedClientId();

		const bool wantShow = (focused != 0) &&
		                      (s.attachMode != Overlay::AttachMode::None);
		if (!wantShow) {
			ctx.overlay->HideOverlay(g_handle);
			return;
		}

		ID3D11Texture2D* panelTex = HelperImpl::GetSingleton().GetClientPanelTexture(focused);
		if (!panelTex) {
			ctx.overlay->HideOverlay(g_handle);
			return;
		}

		// While dragging, OverlayTinter has already copied the panel into a
		// helper-owned texture with the highlight tint baked in (called from
		// HelperImpl::DispatchFrame just before us). Sample that one so the
		// drag feedback is visible. Otherwise hand SteamVR the raw panel.
		const bool dragging = overlayState.dragState.dragging && s.enableDragToReposition;
		ID3D11Texture2D* sourceTex = panelTex;
		if (dragging) {
			if (auto* tinted = OverlayTinter::GetOutputTexture()) {
				sourceTex = tinted;
			}
		}

		// MVP: HMD-relative positioning only. Controller-relative and
		// fixed-world come back in a follow-up commit; the helper-side
		// state for both is preserved (Overlay::State::fixedWorld,
		// Settings::positioningMethod) so reactivating them is a localized
		// change.
		vr::HmdMatrix34_t transform = Util::ComputeOverlayTransformFromHMD(
			s.hmdOffsetX, s.hmdOffsetY, s.hmdOffsetZ);
		ctx.overlay->SetOverlayTransformAbsolute(
			g_handle, vr::TrackingUniverseStanding, &transform);
		ctx.overlay->SetOverlayWidthInMeters(g_handle, s.menuScale);

		if (cleanOverlay) {
			vr::Texture_t tex{};
			tex.handle = sourceTex;
			tex.eType = vr::TextureType_DirectX;
			tex.eColorSpace = vr::ColorSpace_Auto;
			const auto err = cleanOverlay->SetOverlayTexture(g_handle, &tex);
			if (err != vr::VROverlayError_None) {
				if (g_setTextureErrLogCount < 5) {
					logs::warn("OverlayManager: SetOverlayTexture err={} (handle=0x{:X})",
						static_cast<int>(err), g_handle);
					++g_setTextureErrLogCount;
				}
			}
		}

		ctx.overlay->ShowOverlay(g_handle);
	}

	void Shutdown()
	{
		if (g_handle == vr::k_ulOverlayHandleInvalid)
			return;
		Util::OpenVRContext ctx;
		if (ctx.HasOverlay()) {
			ctx.overlay->DestroyOverlay(g_handle);
		}
		g_handle = vr::k_ulOverlayHandleInvalid;
		logs::info("OverlayManager::Shutdown: overlay handle destroyed");
	}
}
