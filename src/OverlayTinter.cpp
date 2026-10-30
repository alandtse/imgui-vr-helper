// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include "pch.h"

#include "OverlayTinter.h"

#include "Globals.h"
#include "Overlay.h"
#include "internal/D3D11StateGuard.h"

#include <algorithm>
#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")

namespace ImGuiVRHelper::OverlayTinter
{
	namespace
	{
		// 8×8 threads per group; the group count is derived from the panel size at
		// dispatch. Standard pattern for full-screen image post-processing.
		constexpr const char* kComputeShader = R"(
cbuffer TintBuffer : register(b0)
{
	float4 tint;  // rgb = tint color, a = lerp factor toward tint
};
Texture2D<float4>     SrcPanel : register(t0);
RWTexture2D<float4>   DstPanel : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
	uint2 dim;
	DstPanel.GetDimensions(dim.x, dim.y);
	if (tid.x >= dim.x || tid.y >= dim.y) return;

	float4 c = SrcPanel.Load(int3(tid.xy, 0));
	c.rgb = lerp(c.rgb, tint.rgb, tint.a);
	DstPanel[tid.xy] = c;
}
)";

		struct CBData
		{
			float tint[4];
		};

		struct Resources
		{
			winrt::com_ptr<ID3D11ComputeShader> cs;
			winrt::com_ptr<ID3D11Texture2D> dstTex;
			winrt::com_ptr<ID3D11UnorderedAccessView> dstUAV;
			winrt::com_ptr<ID3D11ShaderResourceView> dstSRV;
			winrt::com_ptr<ID3D11Buffer> cb;
			UINT dstWidth = 0;
			UINT dstHeight = 0;
			bool initialized = false;
			winrt::com_ptr<ID3D11Texture2D> outputSource;
		};

		Resources g_res;
	}

	bool EnsureResources()
	{
		if (g_res.initialized)
			return true;
		if (!Globals::IsReady())
			return false;
		auto* device = Globals::GetD3D().device;

		g_res.cs = nullptr;
		g_res.cb = nullptr;

		// Compile the compute shader.
		winrt::com_ptr<ID3DBlob> csBlob, errorBlob;
		HRESULT hr = D3DCompile(
			kComputeShader, std::strlen(kComputeShader), "OverlayTinter.cs",
			nullptr, nullptr, "main", "cs_5_0",
			D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
			csBlob.put(), errorBlob.put());
		if (FAILED(hr)) {
			logs::error("OverlayTinter CS compile failed: {}",
				errorBlob ? static_cast<const char*>(errorBlob->GetBufferPointer()) : "<no message>");
			return false;
		}
		if (FAILED(device->CreateComputeShader(csBlob->GetBufferPointer(),
				csBlob->GetBufferSize(), nullptr, g_res.cs.put()))) {
			logs::error("OverlayTinter: CreateComputeShader failed");
			return false;
		}

		D3D11_BUFFER_DESC cbDesc{};
		cbDesc.Usage = D3D11_USAGE_DYNAMIC;
		cbDesc.ByteWidth = sizeof(CBData);
		cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(device->CreateBuffer(&cbDesc, nullptr, g_res.cb.put()))) {
			logs::error("OverlayTinter: constant buffer creation failed");
			return false;
		}

		g_res.initialized = true;
		logs::info("OverlayTinter resources initialized");
		return true;
	}

	// (Re)create the post-process texture to match the focused panel's pixel size.
	// The panel is supersampled (and clients can differ in size), so sizing this to
	// a fixed resolution would tint only a sub-rect — the drag preview would show a
	// cropped, zoomed view that snaps to the full panel on release.
	bool EnsureDstTexture(UINT width, UINT height)
	{
		if (width == 0 || height == 0)
			return false;
		if (g_res.dstTex && g_res.dstWidth == width && g_res.dstHeight == height)
			return true;

		g_res.dstSRV = nullptr;
		g_res.dstUAV = nullptr;
		g_res.dstTex = nullptr;
		g_res.dstWidth = 0;
		g_res.dstHeight = 0;

		auto* device = Globals::GetD3D().device;
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
		if (FAILED(device->CreateTexture2D(&desc, nullptr, g_res.dstTex.put()))) {
			logs::error("OverlayTinter: post-process texture creation failed");
			return false;
		}
		if (FAILED(device->CreateUnorderedAccessView(g_res.dstTex.get(), nullptr, g_res.dstUAV.put()))) {
			logs::error("OverlayTinter: UAV creation failed");
			return false;
		}
		if (FAILED(device->CreateShaderResourceView(g_res.dstTex.get(), nullptr, g_res.dstSRV.put()))) {
			logs::error("OverlayTinter: SRV creation failed");
			return false;
		}
		g_res.dstWidth = width;
		g_res.dstHeight = height;
		return true;
	}

	void InvalidateOutput()
	{
		g_res.outputSource = nullptr;
	}

	void Dispatch(ID3D11Texture2D* srcPanel, const float tint[4])
	{
		InvalidateOutput();
		if (!srcPanel)
			return;
		if (!EnsureResources())
			return;

		// Match the post-process output to the source panel so the tinted drag
		// preview samples the same extent the steady-state quad does.
		D3D11_TEXTURE2D_DESC srcDesc{};
		srcPanel->GetDesc(&srcDesc);
		if (!EnsureDstTexture(srcDesc.Width, srcDesc.Height))
			return;

		auto* ctx = Globals::GetD3D().context;
		auto* device = Globals::GetD3D().device;

		// Need a transient SRV on the source. The client panel was created
		// with BIND_SHADER_RESOURCE; build the view here rather than
		// caching since the focused client can change between frames.
		winrt::com_ptr<ID3D11ShaderResourceView> srcSRV;
		if (FAILED(device->CreateShaderResourceView(srcPanel, nullptr, srcSRV.put()))) {
			return;
		}

		Util::D3D11StateGuard stateGuard(ctx);
		if (!stateGuard.IsValid()) {
			static bool warned = false;
			if (!warned) {
				logs::warn("OverlayTinter: D3D11 state isolation unavailable; skipping tint dispatch");
				warned = true;
			}
			return;
		}

		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(ctx->Map(g_res.cb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			return;
		CBData data{};
		std::copy_n(tint, 4, data.tint);
		std::memcpy(mapped.pData, &data, sizeof(data));
		ctx->Unmap(g_res.cb.get(), 0);

		ID3D11ShaderResourceView* srvPtr = srcSRV.get();
		ID3D11UnorderedAccessView* uavPtr = g_res.dstUAV.get();
		ID3D11Buffer* cbPtr = g_res.cb.get();
		ctx->CSSetShader(g_res.cs.get(), nullptr, 0);
		ctx->CSSetShaderResources(0, 1, &srvPtr);
		ctx->CSSetUnorderedAccessViews(0, 1, &uavPtr, nullptr);
		ctx->CSSetConstantBuffers(0, 1, &cbPtr);
		ctx->Dispatch((srcDesc.Width + 7) / 8, (srcDesc.Height + 7) / 8, 1);
		g_res.outputSource.copy_from(srcPanel);
	}

	ID3D11ShaderResourceView* GetOutputSRV(ID3D11Texture2D* expectedSource)
	{
		return expectedSource && g_res.outputSource.get() == expectedSource ? g_res.dstSRV.get() : nullptr;
	}

	winrt::com_ptr<ID3D11Texture2D> GetOutputTexture(ID3D11Texture2D* expectedSource)
	{
		winrt::com_ptr<ID3D11Texture2D> panel;
		if (expectedSource && g_res.outputSource.get() == expectedSource)
			panel.copy_from(g_res.dstTex.get());
		return panel;
	}
}
