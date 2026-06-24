// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include "pch.h"

#include "ToastHUD.h"

#include "Globals.h"
#include "HudContext.h"
#include "HudRender.h"
#include "Input.h"
#include "Overlay.h"
#include "PluginVersion.h"
#include "Theme.h"

#include <algorithm>

namespace ImGuiVRHelper::ToastHUD
{
	namespace
	{
		// Dedicated, non-interactive ImGui context (own font atlas + DX11 backend,
		// per-context in imgui 1.92) so it never touches the menu contexts.
		ImGuiContext* g_ctx = nullptr;

		bool EnsureInitialized()
		{
			if (g_ctx)
				return true;
			if (!Globals::IsReady())
				return false;
			g_ctx = CreateHudContext("ToastHUD");
			return g_ctx != nullptr;
		}
	}

	void Render(ID3D11RenderTargetView* rtv, const std::string& text, float alpha, float fontScale)
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
		// regardless of name length. Vertical placement is user-configurable.
		const float topFraction =
			std::clamp(Overlay::State::GetSingleton().settings.toastTopFraction, 0.0f, 1.0f);
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

		RenderDrawDataToRtv(rtv);

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

		ImGuiContext* prev = ImGui::GetCurrentContext();
		ImGui::SetCurrentContext(g_ctx);

		ImGuiIO& io = ImGui::GetIO();
		io.DeltaTime = 1.0f / 60.0f;

		ImGui_ImplDX11_NewFrame();
		ImGui::NewFrame();

		alpha = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
		const float welcomeTop =
			std::clamp(Overlay::State::GetSingleton().settings.toastTopFraction, 0.0f, 1.0f);
		ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * welcomeTop),
			ImGuiCond_Always, ImVec2(0.5f, 0.0f));
		ImGui::SetNextWindowBgAlpha(0.65f);
		constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
		                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
		                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
		                                   ImGuiWindowFlags_AlwaysAutoResize;
		if (ImGui::Begin("##Welcome", nullptr, flags)) {
			ImGui::SetWindowFontScale(1.2f);
			ImGui::TextUnformatted("ImGuiVRHelper v" IMGUI_VR_HELPER_VERSION_STRING " ready");
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
					ImGui::TextColored(Theme::DeviceColor(openKeys[i].GetDevice()), "%s (%s)",
						Input::ButtonName(openKeys[i].GetKey()), Theme::DeviceName(openKeys[i].GetDevice()));
				}
			}
			if (pauseHint)
				ImGui::TextDisabled("Opens while the game is paused (in a menu).");
			ImGui::TextDisabled("Settings: Shift+F4 (or the open combo with no mod open).");
			ImGui::TextDisabled("Switch mods: Settings -> Active overlay.");
		}
		ImGui::End();
		ImGui::PopStyleVar();

		ImGui::Render();
		RenderDrawDataToRtv(rtv);

		if (prev != g_ctx)
			ImGui::SetCurrentContext(prev);
	}

	void Shutdown()
	{
		if (g_ctx) {
			ImGui::SetCurrentContext(g_ctx);
			ImGui_ImplDX11_Shutdown();
			ImGui::DestroyContext(g_ctx);
			g_ctx = nullptr;
		}
	}
}
