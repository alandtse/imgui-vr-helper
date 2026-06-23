// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include "pch.h"

#include "ToastHUD.h"

#include "Globals.h"
#include "Overlay.h"
#include "Theme.h"

namespace ImGuiVRHelper::ToastHUD
{
	namespace
	{
		// Dedicated, non-interactive ImGui context (own font atlas + DX11 backend,
		// per-context in imgui 1.92) so it never touches the menu contexts.
		ImGuiContext* g_ctx = nullptr;
		bool g_dx11Inited = false;

		// Matches the panel RTV the helper hands every client; the HUD pass
		// composites this fullscreen onto each eye.
		constexpr float kPanelWidth = static_cast<float>(Overlay::Config::kOverlayWidth);
		constexpr float kPanelHeight = static_cast<float>(Overlay::Config::kOverlayHeight);

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
				io.IniFilename = nullptr;
				io.LogFilename = nullptr;
				io.DisplaySize = ImVec2(kPanelWidth, kPanelHeight);
				Theme::Apply(ImGui::GetStyle());  // match the Community Shaders look
				ImGui::GetStyle().ScaleAllSizes(2.0f);
				io.FontGlobalScale = 1.5f;
			}

			if (!g_dx11Inited) {
				ImGui::SetCurrentContext(g_ctx);
				auto& d3d = Globals::GetD3D();
				if (!ImGui_ImplDX11_Init(d3d.device, d3d.context)) {
					logs::error("ToastHUD: ImGui_ImplDX11_Init failed");
					return false;
				}
				g_dx11Inited = true;
			}
			return true;
		}
	}

	void Render(ID3D11RenderTargetView* rtv, const std::string& text, float alpha,
		float topFraction, float fontScale)
	{
		if (!rtv || text.empty())
			return;
		if (!EnsureInitialized())
			return;

		ImGuiContext* prev = ImGui::GetCurrentContext();
		ImGui::SetCurrentContext(g_ctx);

		ImGuiIO& io = ImGui::GetIO();
		io.DeltaTime = 1.0f / 60.0f;  // static content; dt only matters for animation

		ImGui_ImplDX11_NewFrame();
		ImGui::NewFrame();

		alpha = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);  // fades text + background together

		// Pivot the window by its top-center so the banner stays centered
		// regardless of name length.
		ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * topFraction),
			ImGuiCond_Always, ImVec2(0.5f, 0.0f));
		ImGui::SetNextWindowBgAlpha(0.65f);
		constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
		                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
		                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
		                                   ImGuiWindowFlags_AlwaysAutoResize;
		if (ImGui::Begin("##SwapToast", nullptr, flags)) {
			ImGui::SetWindowFontScale(fontScale);
			ImGui::TextUnformatted(text.c_str());
		}
		ImGui::End();
		ImGui::PopStyleVar();

		ImGui::Render();

		// Render into the helper-owned panel RTV; save/restore the bound target so
		// the surrounding compositing chain isn't disturbed.
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

		if (prev != g_ctx)
			ImGui::SetCurrentContext(prev);
	}

	void RenderWelcome(ID3D11RenderTargetView* rtv, float alpha,
		const std::vector<ImGuiVRHelperPluginAPI::InputCombo>& openKeys, bool pauseHint)
	{
		if (!rtv)
			return;
		if (!EnsureInitialized())
			return;

		auto keyName = [](uint32_t k) -> const char* {
			switch (k) {
			case 1:
				return "B/Y";
			case 2:
				return "Grip";
			case 7:
				return "A/X";
			case 32:
				return "Stick";
			case 33:
				return "Trigger";
			case 34:
				return "Grip";
			case 35:
				return "Touchpad";
			default:
				return "?";
			}
		};

		ImGuiContext* prev = ImGui::GetCurrentContext();
		ImGui::SetCurrentContext(g_ctx);

		ImGuiIO& io = ImGui::GetIO();
		io.DeltaTime = 1.0f / 60.0f;

		ImGui_ImplDX11_NewFrame();
		ImGui::NewFrame();

		alpha = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
		ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.18f),
			ImGuiCond_Always, ImVec2(0.5f, 0.0f));
		ImGui::SetNextWindowBgAlpha(0.65f);
		constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
		                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
		                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
		                                   ImGuiWindowFlags_AlwaysAutoResize;
		if (ImGui::Begin("##Welcome", nullptr, flags)) {
			ImGui::SetWindowFontScale(1.2f);
			ImGui::TextUnformatted("ImGuiVRHelper ready");
			ImGui::TextUnformatted("Open menu:");
			if (openKeys.empty()) {
				ImGui::SameLine();
				ImGui::TextDisabled("(unbound)");
			} else {
				for (std::size_t i = 0; i < openKeys.size(); ++i) {
					ImGui::SameLine();
					if (i != 0) {
						ImGui::TextDisabled("+");
						ImGui::SameLine();
					}
					ImGui::TextColored(Theme::DeviceColor(openKeys[i].GetDevice()), "%s",
						keyName(openKeys[i].GetKey()));
				}
			}
			if (pauseHint)
				ImGui::TextDisabled("Opens while the game is paused (in a menu).");
			ImGui::TextDisabled("Settings: Shift+F4");
		}
		ImGui::End();
		ImGui::PopStyleVar();

		ImGui::Render();

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

		if (prev != g_ctx)
			ImGui::SetCurrentContext(prev);
	}

	void ClearToTransparent(ID3D11RenderTargetView* rtv)
	{
		if (!rtv || !Globals::IsReady())
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
