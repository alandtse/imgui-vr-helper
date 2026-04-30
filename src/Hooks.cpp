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
#include "Input.h"
#include "SettingsUI.h"

#include <RE/B/BSOpenVRControllerDevice.h>
#include <RE/B/ButtonEvent.h>
#include <RE/I/InputEvent.h>
#include <RE/R/Renderer.h>
#include <RE/T/ThumbstickEvent.h>

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

		// ---- BSInputDeviceManager::PollInputDevices thunk ---------------
		//
		// Walks the InputEvent linked list, feeds VR button/thumbstick
		// events into Input::FeedVREvent so Overlay::State's controller
		// state stays current. Chains to the original (and any other
		// plugin's thunk), so nothing else is disturbed.

		bool IsVRControllerDevice(RE::INPUT_DEVICE d)
		{
			switch (d) {
			case RE::INPUT_DEVICE::kVivePrimary:
			case RE::INPUT_DEVICE::kViveSecondary:
			case RE::INPUT_DEVICE::kOculusPrimary:
			case RE::INPUT_DEVICE::kOculusSecondary:
			case RE::INPUT_DEVICE::kWMRPrimary:
			case RE::INPUT_DEVICE::kWMRSecondary:
				return true;
			default:
				return false;
			}
		}

		// DirectInput scan code for F2. Used as the default keyboard
		// toggle for the helper's settings UI. Hardcoded for now —
		// keyboard input is the only reliable way to reach the helper
		// at the main menu, where VR controller buttons (BY/AX/trigger)
		// drive menu navigation and may be consumed before reaching us.
		// Future: make this user-configurable via the TOML config.
		constexpr std::uint32_t kKeyboardToggleDIK = 0x3C;  // DIK_F2

		struct PollInputDevices_t
		{
			static void thunk(RE::BSTEventSource<RE::InputEvent*>* a_dispatcher,
				RE::InputEvent* const* a_events)
			{
				if (a_events) {
					for (auto* e = *a_events; e; e = e->next) {
						const auto device = e->GetDevice();

						// Keyboard fallback toggle (F2) — fires the same
						// SettingsUI::Toggle path the controller combo
						// uses, so it works at the main menu where
						// controller buttons may be intercepted.
						if (device == RE::INPUT_DEVICE::kKeyboard &&
							e->GetEventType() == RE::INPUT_EVENT_TYPE::kButton) {
							if (auto* btn = e->AsButtonEvent();
								btn && btn->GetIDCode() == kKeyboardToggleDIK &&
								btn->IsDown()) {
								HelperImpl::GetSingleton().OnKeyboardToggle();
							}
							continue;
						}

						if (!IsVRControllerDevice(device))
							continue;

						switch (e->GetEventType()) {
						case RE::INPUT_EVENT_TYPE::kButton:
							{
								if (auto* btn = e->AsButtonEvent()) {
									Input::FeedVREvent(static_cast<uint32_t>(device),
										btn->GetIDCode(),
										btn->IsPressed(),
										0.0f, 0.0f);
								}
								break;
							}
						case RE::INPUT_EVENT_TYPE::kThumbstick:
							{
								if (auto* ts = e->AsThumbstickEvent()) {
									Input::FeedVREvent(static_cast<uint32_t>(device),
										ts->GetIDCode(),
										false,
										ts->xValue, ts->yValue);
								}
								break;
							}
						default:
							break;
						}
					}
				}
				func(a_dispatcher, a_events);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

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

		SKSE::AllocTrampoline(28);

		logs::info("Hooks::Install - writing BSGraphics::Renderer::InitD3D thunk");
		// VR offset 77226 + 0x2BC matches the SCS Hooks.cpp install site for VR.
		stl::write_thunk_call<BSGraphics_Renderer_Init_InitD3D>(
			REL::RelocationID(75595, 77226).address() + REL::Relocate(0x50, 0x2BC));

		logs::info("Hooks::Install - writing BSInputDeviceManager::PollInputDevices thunk");
		// Same install site SCS uses; SKSE trampoline chains correctly when
		// multiple plugins hook here.
		stl::write_thunk_call<PollInputDevices_t>(
			REL::RelocationID(67315, 68617).address() + REL::Relocate(0x7B, 0x7B, 0x81));
	}
}
