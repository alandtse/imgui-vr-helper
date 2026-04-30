// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Pattern lifted from SCS's src/Hooks.cpp:
//   stl::write_thunk_call<BSGraphics_Renderer_Init_InitD3D>(
//       REL::RelocationID(75595, 77226).address() + REL::Relocate(0x50, 0x2BC));
//
// Captures the renderer's device/context/swapchain into Globals after
// the original InitD3D returns. Future hook installations (Present detour,
// eye-submit detour for InSceneOverlay) live in this file.

#include "pch.h"

#include "Hooks.h"

#include "Globals.h"
#include "HelperImpl.h"
#include "InSceneOverlay.h"
#include "SettingsUI.h"

#include <RE/R/Renderer.h>

#include <chrono>
#include <dxgi.h>

namespace ImGuiVRHelper::Hooks
{
	namespace
	{
		// ---- Manual COM vtable detour -----------------------------------
		//
		// Equivalent to Detours::X64::DetourClassVTable used by SCS, written
		// inline so we don't need to vendor MS Detours just for this.

		template <class FnPtr>
		FnPtr DetourClassVTable(void* obj, std::size_t slot, FnPtr replacement)
		{
			auto** vtable = *reinterpret_cast<void***>(obj);
			DWORD oldProtect = 0;
			VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &oldProtect);
			FnPtr original = reinterpret_cast<FnPtr>(vtable[slot]);
			vtable[slot] = reinterpret_cast<void*>(replacement);
			VirtualProtect(&vtable[slot], sizeof(void*), oldProtect, &oldProtect);
			return original;
		}

		// ---- IDXGISwapChain::Present detour -----------------------------
		//
		// Slot 8 in the IDXGISwapChain vtable. Drives the helper's per-frame
		// tick: builds a Frame snapshot and dispatches it to each registered
		// client before falling through to the original Present.

		using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
		PresentFn g_originalPresent = nullptr;

		std::chrono::steady_clock::time_point g_lastPresent;
		bool g_lastPresentValid = false;

		HRESULT WINAPI hk_Present(IDXGISwapChain* This, UINT SyncInterval, UINT Flags)
		{
			const auto now = std::chrono::steady_clock::now();
			float dt = 0.016f;  // sane default for the very first frame
			if (g_lastPresentValid) {
				dt = std::chrono::duration<float>(now - g_lastPresent).count();
				dt = std::clamp(dt, 0.0001f, 0.5f);  // guard against pause / huge gaps
			}
			g_lastPresent = now;
			g_lastPresentValid = true;

			HelperImpl::GetSingleton().DispatchFrame(dt);

			return g_originalPresent(This, SyncInterval, Flags);
		}

		// ---- BSGraphics::Renderer::InitD3D thunk ------------------------

		struct BSGraphics_Renderer_Init_InitD3D
		{
			static void thunk()
			{
				logs::info("BSGraphics::Renderer::InitD3D - calling original");
				func();

				auto* manager = RE::BSGraphics::Renderer::GetSingleton();
				if (!manager) {
					logs::warn("InitD3D thunk: BSGraphics::Renderer singleton missing");
					return;
				}

				auto& runtime = manager->GetRuntimeData();
				auto* device = reinterpret_cast<ID3D11Device*>(runtime.forwarder);
				auto* context = reinterpret_cast<ID3D11DeviceContext*>(runtime.context);
				auto* swapchain = reinterpret_cast<IDXGISwapChain*>(runtime.renderWindows[0].swapChain);

				if (!device || !context || !swapchain) {
					logs::warn(
						"InitD3D thunk: incomplete renderer runtime data "
						"(device={}, context={}, swapchain={})",
						static_cast<void*>(device),
						static_cast<void*>(context),
						static_cast<void*>(swapchain));
					return;
				}

				Globals::SetD3D(device, context, swapchain);

				// Now that we have the swapchain, install the Present
				// detour so the helper has a per-frame tick.
				g_originalPresent = DetourClassVTable<PresentFn>(swapchain, 8, &hk_Present);
				logs::info("IDXGISwapChain::Present detour installed (original={})",
					reinterpret_cast<void*>(g_originalPresent));

				// Install the IVRCompositor::Submit detour so the helper
				// composites the focused client's panel into each eye
				// render target. Universal across SteamVR + OpenComposite.
				InSceneOverlay::Install();

				// Initialize the helper's own ImGui context for its
				// settings panel; register the synthetic self-client so
				// the existing per-client texture pipeline allocates an
				// RTV for it.
				SettingsUI::Init();
				HelperImpl::GetSingleton().EnsureSelfClient();
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		// Skyrim has SE/AE/VR address-library variants. The helper is VR-only
		// for now, so we use the VR offset directly. If the helper ever
		// becomes multi-runtime, extend with REL::RelocationID(SE, AE, VR).
		if (!REL::Module::IsVR()) {
			logs::warn("Hooks::Install skipped: not running in Skyrim VR");
			return;
		}

		SKSE::AllocTrampoline(14);

		logs::info("Hooks::Install - writing BSGraphics::Renderer::InitD3D thunk");
		// VR offset 77226 + 0x2BC matches the SCS Hooks.cpp install site for VR.
		stl::write_thunk_call<BSGraphics_Renderer_Init_InitD3D>(
			REL::RelocationID(75595, 77226).address() + REL::Relocate(0x50, 0x2BC));
	}
}
