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

	void Render(ID3D11RenderTargetView* rtv, unsigned int panelWidth, unsigned int panelHeight)
	{
		if (!rtv)
			return;
		if (!EnsureInitialized())
			return;

		ImGuiContext* prev = ImGui::GetCurrentContext();
		ImGui::SetCurrentContext(g_ctx);

		ImGuiIO& io = ImGui::GetIO();
		io.DeltaTime = 1.0f / 60.0f;  // demo content is static; dt only matters for animation

		// Grid is laid out in the logical base resolution; DisplayFramebufferScale
		// stretches it to fill the (possibly supersampled) panel — same high-DPI
		// setup as the other HUD layers, so the grid spans the whole view.
		const auto& s = Overlay::State::GetSingleton().settings;
		const float baseW = s.baseWidth > 0 ? static_cast<float>(s.baseWidth) : kPanelWidth;
		const float baseH = s.baseHeight > 0 ? static_cast<float>(s.baseHeight) : kPanelHeight;
		io.DisplaySize = ImVec2(baseW, baseH);
		if (panelWidth && panelHeight && baseW > 0.0f && baseH > 0.0f)
			io.DisplayFramebufferScale =
				ImVec2(static_cast<float>(panelWidth) / baseW, static_cast<float>(panelHeight) / baseH);

		ImGui_ImplDX11_NewFrame();
		ImGui::NewFrame();

		// Calibration grid drawn on the background drawlist. Far more
		// useful for HUD pipeline debugging than free-form Lorem Ipsum:
		//
		//   - Outer border at exact panel bounds (0,0)-(1920,1080):
		//     confirms the full RTV reaches the eye buffer without the
		//     3D-quad-at-HMD-depth math cropping edges.
		//   - Major gridlines every 240px (8 vertical, 4-5 horizontal):
		//     plotted with labels so a HUD client author can verify
		//     "if I draw at (960, 540), it lands at the center."
		//   - Minor gridlines every 60px: visible spatial reference
		//     for finer-grained positioning.
		//   - Center crosshair: quick visual check the geometric center
		//     is where you expect (no off-axis stretching from the
		//     quad's per-eye projection).
		//   - Diagonal X across the full panel: confirms aspect ratio
		//     is preserved end-to-end — if the diagonal looks straight
		//     in the headset, neither the 3D quad's scaling nor the
		//     eye-buffer composite is squashing one axis.
		auto* dl = ImGui::GetBackgroundDrawList();
		const float w = io.DisplaySize.x;
		const float h = io.DisplaySize.y;
		const ImU32 colMinor = IM_COL32(120, 120, 160, 90);
		const ImU32 colMajor = IM_COL32(180, 220, 255, 200);
		const ImU32 colBorder = IM_COL32(255, 200, 80, 220);
		const ImU32 colCenter = IM_COL32(255, 100, 100, 230);
		const ImU32 colDiagonal = IM_COL32(120, 255, 120, 130);
		const ImU32 colLabel = IM_COL32(255, 230, 120, 230);

		// Minor gridlines every 60px.
		constexpr float kMinorStep = 60.0f;
		for (float x = kMinorStep; x < w; x += kMinorStep) {
			dl->AddLine(ImVec2(x, 0), ImVec2(x, h), colMinor, 1.0f);
		}
		for (float y = kMinorStep; y < h; y += kMinorStep) {
			dl->AddLine(ImVec2(0, y), ImVec2(w, y), colMinor, 1.0f);
		}

		// Major gridlines every 240px with labels.
		constexpr float kMajorStep = 240.0f;
		char label[16];
		for (float x = kMajorStep; x < w; x += kMajorStep) {
			dl->AddLine(ImVec2(x, 0), ImVec2(x, h), colMajor, 2.0f);
			std::snprintf(label, sizeof(label), "%d", static_cast<int>(x));
			dl->AddText(ImVec2(x + 4.0f, 4.0f), colLabel, label);
		}
		for (float y = kMajorStep; y < h; y += kMajorStep) {
			dl->AddLine(ImVec2(0, y), ImVec2(w, y), colMajor, 2.0f);
			std::snprintf(label, sizeof(label), "%d", static_cast<int>(y));
			dl->AddText(ImVec2(4.0f, y + 2.0f), colLabel, label);
		}

		// Diagonal X confirms aspect ratio integrity.
		dl->AddLine(ImVec2(0, 0), ImVec2(w, h), colDiagonal, 1.5f);
		dl->AddLine(ImVec2(w, 0), ImVec2(0, h), colDiagonal, 1.5f);

		// Outer panel border (sits at the exact RTV edge).
		dl->AddRect(ImVec2(0.5f, 0.5f), ImVec2(w - 0.5f, h - 0.5f),
			colBorder, 0, 0, 3.0f);

		// Center crosshair + filled dot.
		const ImVec2 ctr(w * 0.5f, h * 0.5f);
		dl->AddLine(ImVec2(ctr.x - 40, ctr.y), ImVec2(ctr.x + 40, ctr.y), colCenter, 2.0f);
		dl->AddLine(ImVec2(ctr.x, ctr.y - 40), ImVec2(ctr.x, ctr.y + 40), colCenter, 2.0f);
		dl->AddCircleFilled(ctr, 6.0f, colCenter);
		std::snprintf(label, sizeof(label), "(%d, %d)", static_cast<int>(ctr.x), static_cast<int>(ctr.y));
		dl->AddText(ImVec2(ctr.x + 12, ctr.y - 24), colCenter, label);

		// Resolution + scale info window in the upper-left, kept small
		// so it doesn't dominate the calibration view. NoBackground +
		// NoInputs so it's purely informational.
		ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
		ImGui::SetNextWindowBgAlpha(0.6f);
		if (ImGui::Begin("HUDDemoInfo", nullptr,
				ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
					ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
					ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
					ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
				"HUDMode smoke test — calibration grid");
			ImGui::Separator();
			ImGui::Text("Panel: %.0f x %.0f", w, h);
			ImGui::Text("Major lines: every %.0f px", kMajorStep);
			ImGui::Text("Minor lines: every %.0f px", kMinorStep);
			ImGui::Spacing();
			ImGui::TextDisabled("Toggle off in Settings -> Diagnostics.");
			(void)kLoremIpsum;  // kept around for potential future text test
		}
		ImGui::End();

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
