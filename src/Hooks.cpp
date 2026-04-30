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

#include <RE/R/Renderer.h>

namespace ImGuiVRHelper::Hooks
{
	namespace
	{
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
