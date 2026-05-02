// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include "pch.h"

#include "HUDDemo.h"

#include "Globals.h"
#include "Overlay.h"

namespace ImGuiVRHelper::HUDDemo
{
	namespace
	{
		ImGuiContext* g_ctx = nullptr;
		bool g_dx11Inited = false;

		// Fixed dimensions matching the panel RTV the helper hands every
		// client. The HUD pass composites this fullscreen onto each eye,
		// so DisplaySize matches the texture dimensions exactly.
		constexpr float kPanelWidth = static_cast<float>(Overlay::Config::kOverlayWidth);
		constexpr float kPanelHeight = static_cast<float>(Overlay::Config::kOverlayHeight);

		const char* kLoremIpsum =
			"Lorem ipsum dolor sit amet, consectetur adipiscing elit. "
			"Sed do eiusmod tempor incididunt ut labore et dolore magna "
			"aliqua. Ut enim ad minim veniam, quis nostrud exercitation "
			"ullamco laboris nisi ut aliquip ex ea commodo consequat.";
	}

	bool EnsureInitialized()
	{
		if (g_ctx && g_dx11Inited)
			return true;
		if (!Globals::IsReady())
			return false;

		if (!g_ctx) {
			IMGUI_CHECKVERSION();
			g_ctx = ImGui::CreateContext();
			ImGui::SetCurrentContext(g_ctx);

			ImGuiIO& io = ImGui::GetIO();
			io.IniFilename = nullptr;  // demo doesn't persist any state
			io.LogFilename = nullptr;
			io.DisplaySize = ImVec2(kPanelWidth, kPanelHeight);
			ImGui::GetStyle().ScaleAllSizes(2.0f);
			io.FontGlobalScale = 1.5f;
		}

		if (!g_dx11Inited) {
			ImGui::SetCurrentContext(g_ctx);
			auto& d3d = Globals::GetD3D();
			if (!ImGui_ImplDX11_Init(d3d.device, d3d.context)) {
				logs::error("HUDDemo: ImGui_ImplDX11_Init failed");
				return false;
			}
			g_dx11Inited = true;
			logs::info("HUDDemo initialized (separate ImGui context @ {})",
				static_cast<void*>(g_ctx));
		}
		return true;
	}

	void Render(ID3D11RenderTargetView* rtv)
	{
		if (!rtv)
			return;
		if (!EnsureInitialized())
			return;

		ImGuiContext* prev = ImGui::GetCurrentContext();
		ImGui::SetCurrentContext(g_ctx);

		ImGuiIO& io = ImGui::GetIO();
		io.DeltaTime = 1.0f / 60.0f;  // demo content is static; dt only matters for animation

		ImGui_ImplDX11_NewFrame();
		ImGui::NewFrame();

		// Demo window centered horizontally, in the upper third of the
		// viewport. ImGuiWindowFlags chosen so the window can't take
		// focus (no input routes to it — it's purely visual) and
		// doesn't try to persist position.
		ImGui::SetNextWindowPos(
			ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.18f),
			ImGuiCond_Always, ImVec2(0.5f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(900, 0), ImGuiCond_Always);
		ImGui::SetNextWindowBgAlpha(0.55f);
		if (ImGui::Begin("HUDDemoWindow", nullptr,
				ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
					ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
					ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
					ImGuiWindowFlags_NoCollapse)) {
			ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
				"ImGuiVRHelper — kClientFlag_HUDMode smoke test");
			ImGui::Separator();
			ImGui::Spacing();
			ImGui::TextWrapped("%s", kLoremIpsum);
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f),
				"If you can read this through your headset,");
			ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f),
				"the HUD compositing pipeline works end-to-end.");
			ImGui::Spacing();
			ImGui::TextDisabled("Toggle off in helper Settings -> Diagnostics.");
		}
		ImGui::End();

		// A few shapes on the background drawlist — at the bottom of
		// the viewport, away from the window so they're independently
		// visible. Mix of color/alpha values so the user can confirm
		// alpha blending is correctly weighted (not just opaque-or-
		// nothing).
		auto* dl = ImGui::GetBackgroundDrawList();
		const ImVec2 anchor(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.78f);
		dl->AddCircle(ImVec2(anchor.x - 220, anchor.y), 60.0f,
			IM_COL32(80, 220, 255, 220), 0, 6.0f);
		dl->AddCircleFilled(ImVec2(anchor.x - 220, anchor.y), 30.0f,
			IM_COL32(80, 220, 255, 100));
		dl->AddRect(ImVec2(anchor.x - 60, anchor.y - 60),
			ImVec2(anchor.x + 60, anchor.y + 60),
			IM_COL32(255, 220, 80, 220), 0, 0, 6.0f);
		dl->AddRectFilled(ImVec2(anchor.x - 30, anchor.y - 30),
			ImVec2(anchor.x + 30, anchor.y + 30),
			IM_COL32(255, 220, 80, 100));
		dl->AddLine(ImVec2(anchor.x + 100, anchor.y - 60),
			ImVec2(anchor.x + 280, anchor.y + 60),
			IM_COL32(255, 100, 200, 255), 6.0f);
		dl->AddLine(ImVec2(anchor.x + 280, anchor.y - 60),
			ImVec2(anchor.x + 100, anchor.y + 60),
			IM_COL32(255, 100, 200, 255), 6.0f);

		ImGui::Render();

		// Render the demo's draw data into the helper-owned panel RTV.
		// Save/restore the bound RTV so we don't leave Skyrim's pipeline
		// disturbed (Submit hook fires after this; we want it to find
		// the same render targets bound that it had before).
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

		if (prev != g_ctx) {
			ImGui::SetCurrentContext(prev);
		}
	}

	void ClearToTransparent(ID3D11RenderTargetView* rtv)
	{
		if (!rtv)
			return;
		if (!Globals::IsReady())
			return;
		const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		Globals::GetD3D().context->ClearRenderTargetView(rtv, clear);
	}

	void Shutdown()
	{
		if (g_dx11Inited && g_ctx) {
			ImGui::SetCurrentContext(g_ctx);
			ImGui_ImplDX11_Shutdown();
			g_dx11Inited = false;
		}
		if (g_ctx) {
			ImGui::DestroyContext(g_ctx);
			g_ctx = nullptr;
		}
	}
}
