// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include "pch.h"

#include "InSceneOcclusion.h"

#include "Globals.h"
#include "HelperImpl.h"
#include "InSceneOverlay.h"
#include "Overlay.h"
#include "internal/D3D11StateGuard.h"
#include "internal/HUDGeometry.h"
#include "internal/HUDOcclusion.h"
#include "internal/VRUtils.h"

#include <RE/B/BSBatchRenderer.h>
#include <RE/B/BSShaderRenderTargets.h>
#include <RE/R/Renderer.h>
#include <RE/R/RendererShadowState.h>
#include <RE/S/State.h>

#include <DirectXMath.h>
#include <SimpleMath.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

namespace ImGuiVRHelper::HUDOcclusion
{
	using DirectX::SimpleMath::Matrix;

	namespace
	{
		constexpr UINT kHudStencilRef = 0xFF;

		constexpr const char* kVertexShader = R"(
cbuffer MatrixBuffer : register(b0)
{
	matrix wvp;
	float2 uvScale;
	float2 uvOffset;
};
struct VS_INPUT
{
	float3 pos: POSITION;
	float2 uv: TEXCOORD0;
};
struct PS_INPUT
{
	float4 pos: SV_POSITION;
	float2 uv: TEXCOORD0;
};
PS_INPUT main(VS_INPUT input)
{
	PS_INPUT output;
	output.pos = mul(float4(input.pos, 1.0f), wvp);
	output.uv = input.uv * uvScale + uvOffset;
	return output;
}
)";

		constexpr const char* kPixelShader = R"(
Texture2D layerTexture : register(t0);
SamplerState layerSampler : register(s0);
cbuffer DepthBuffer : register(b1)
{
	float planeDepth;
	float opacityThreshold;
	float2 depthPad;
};
struct PS_INPUT
{
	float4 pos: SV_POSITION;
	float2 uv: TEXCOORD0;
};
float main(PS_INPUT input) : SV_Depth
{
	if (layerTexture.SampleLevel(layerSampler, input.uv, 0).a < opacityThreshold)
		discard;
	return planeDepth;
}
)";

		struct QuadCB
		{
			Matrix wvp;
			float uvScale[2] = { 1.0f, 1.0f };
			float uvOffset[2] = { 0.0f, 0.0f };
		};

		struct DepthCB
		{
			float planeDepth = 0.0f;
			float opacityThreshold = kOpaqueAlphaThreshold;
			float depthPad[2] = { 0.0f, 0.0f };
		};

		struct Pipeline
		{
			winrt::com_ptr<ID3D11VertexShader> vs;
			winrt::com_ptr<ID3D11PixelShader> ps;
			winrt::com_ptr<ID3D11Buffer> quadCB;
			winrt::com_ptr<ID3D11Buffer> depthCB;
			winrt::com_ptr<ID3D11Buffer> vb;
			winrt::com_ptr<ID3D11Buffer> ib;
			winrt::com_ptr<ID3D11InputLayout> layout;
			winrt::com_ptr<ID3D11SamplerState> sampler;
			winrt::com_ptr<ID3D11DepthStencilState> depthWrite;
			winrt::com_ptr<ID3D11DepthStencilState> depthStencilWrite;
			winrt::com_ptr<ID3D11RasterizerState> rasterizer;
			winrt::com_ptr<ID3D11BlendState> noColor;
		};

		Pipeline g_pipeline;
		std::atomic<bool> g_installed{ false };
		std::atomic<BackendStatus> g_status{ BackendStatus::Disabled };
		std::atomic<bool> g_pipelineFailed{ false };
		winrt::com_ptr<ID3D11Device> g_attemptedDevice;

		const char* BackendStatusToString(BackendStatus status)
		{
			switch (status) {
			case BackendStatus::Ready:
				return "active";
			case BackendStatus::Disabled:
				return "off";
			case BackendStatus::NoLayers:
				return "idle (no HUD layer this frame)";
			case BackendStatus::NotMainDepth:
				return "unavailable: not drawing the main depth target";
			case BackendStatus::UnusableDepth:
				return "unavailable: camera depth range unusable or projection unrecognised";
			case BackendStatus::NoDevice:
				return "unavailable: D3D not ready";
			}
			return "unknown";
		}

		winrt::com_ptr<ID3DBlob> Compile(const char* source, const char* target, const char* name)
		{
			winrt::com_ptr<ID3DBlob> code;
			winrt::com_ptr<ID3DBlob> errors;
			const HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr,
				"main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), errors.put());
			if (FAILED(hr)) {
				logs::error("HUDOcclusion: {} shader failed: {}", name,
					errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
				return nullptr;
			}
			return code;
		}

		bool CreatePipeline(ID3D11Device* device, Pipeline& pipeline)
		{
			const auto vsBlob = Compile(kVertexShader, "vs_5_0", "vertex");
			const auto psBlob = Compile(kPixelShader, "ps_5_0", "pixel");
			if (!vsBlob || !psBlob)
				return false;
			if (FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
					nullptr, pipeline.vs.put())) ||
				FAILED(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
					nullptr, pipeline.ps.put()))) {
				logs::error("HUDOcclusion: shader creation failed");
				return false;
			}

			const D3D11_INPUT_ELEMENT_DESC elems[] = {
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			};
			if (FAILED(device->CreateInputLayout(elems, 2, vsBlob->GetBufferPointer(),
					vsBlob->GetBufferSize(), pipeline.layout.put()))) {
				logs::error("HUDOcclusion: input layout creation failed");
				return false;
			}

			struct VertexType
			{
				DirectX::XMFLOAT3 position;
				DirectX::XMFLOAT2 texture;
			};
			const VertexType vertices[4] = {
				{ { -0.5f, -0.5f, 0.0f }, { 0.0f, 1.0f } },
				{ { -0.5f, 0.5f, 0.0f }, { 0.0f, 0.0f } },
				{ { 0.5f, 0.5f, 0.0f }, { 1.0f, 0.0f } },
				{ { 0.5f, -0.5f, 0.0f }, { 1.0f, 1.0f } },
			};
			const uint32_t indices[6] = { 0, 1, 2, 0, 2, 3 };

			D3D11_BUFFER_DESC vbDesc{};
			vbDesc.Usage = D3D11_USAGE_DEFAULT;
			vbDesc.ByteWidth = sizeof(vertices);
			vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
			D3D11_SUBRESOURCE_DATA vbData{ vertices };
			D3D11_BUFFER_DESC ibDesc{};
			ibDesc.Usage = D3D11_USAGE_DEFAULT;
			ibDesc.ByteWidth = sizeof(indices);
			ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
			D3D11_SUBRESOURCE_DATA ibData{ indices };
			D3D11_BUFFER_DESC cbDesc{};
			cbDesc.Usage = D3D11_USAGE_DYNAMIC;
			cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

			cbDesc.ByteWidth = sizeof(QuadCB);
			if (FAILED(device->CreateBuffer(&cbDesc, nullptr, pipeline.quadCB.put())))
				return false;
			cbDesc.ByteWidth = sizeof(DepthCB);
			if (FAILED(device->CreateBuffer(&cbDesc, nullptr, pipeline.depthCB.put())))
				return false;
			if (FAILED(device->CreateBuffer(&vbDesc, &vbData, pipeline.vb.put())) ||
				FAILED(device->CreateBuffer(&ibDesc, &ibData, pipeline.ib.put()))) {
				logs::error("HUDOcclusion: geometry buffer creation failed");
				return false;
			}

			D3D11_SAMPLER_DESC samplerDesc{};
			samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
			if (FAILED(device->CreateSamplerState(&samplerDesc, pipeline.sampler.put())))
				return false;

			D3D11_DEPTH_STENCIL_DESC dsDesc{};
			dsDesc.DepthEnable = TRUE;
			dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
			dsDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
			dsDesc.StencilEnable = FALSE;
			if (FAILED(device->CreateDepthStencilState(&dsDesc, pipeline.depthWrite.put())))
				return false;

			D3D11_DEPTH_STENCIL_DESC stencilDesc = dsDesc;
			stencilDesc.StencilEnable = TRUE;
			stencilDesc.StencilReadMask = 0xFF;
			stencilDesc.StencilWriteMask = 0xFF;
			stencilDesc.FrontFace = { D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE,
				D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS };
			stencilDesc.BackFace = stencilDesc.FrontFace;
			if (FAILED(device->CreateDepthStencilState(&stencilDesc, pipeline.depthStencilWrite.put())))
				return false;

			D3D11_RASTERIZER_DESC rasDesc{};
			rasDesc.FillMode = D3D11_FILL_SOLID;
			rasDesc.CullMode = D3D11_CULL_NONE;
			rasDesc.DepthClipEnable = FALSE;
			if (FAILED(device->CreateRasterizerState(&rasDesc, pipeline.rasterizer.put())))
				return false;

			D3D11_BLEND_DESC blendDesc{};
			blendDesc.RenderTarget[0].RenderTargetWriteMask = 0;
			if (FAILED(device->CreateBlendState(&blendDesc, pipeline.noColor.put())))
				return false;

			return true;
		}

		bool ResolveConvention(float camNear, float camFar, HUDOcclusion::DepthConvention& out)
		{
			auto* shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
			if (!shadowState)
				return false;
			const auto eye = shadowState->GetVRRuntimeData().cameraData.getEye(0);
			const auto* m = reinterpret_cast<const float*>(&eye.projMat);
			for (int i = 0; i < 16; ++i) {
				if (!std::isfinite(m[i]))
					return false;
			}
			return InSceneOverlay::ResolveDepthConvention(m, camNear, camFar, out);
		}

		bool DrawsToMainDepth()
		{
			auto* shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
			if (!shadowState)
				return false;
			return shadowState->GetVRRuntimeData().depthStencil ==
			       static_cast<uint32_t>(RE::RENDER_TARGET_DEPTHSTENCIL::kMAIN);
		}

		struct DepthTarget
		{
			ID3D11DepthStencilView* dsv = nullptr;
			ID3D11ShaderResourceView* srv = nullptr;
			uint32_t width = 0;
			uint32_t height = 0;
			float camNear = 0.0f;
			float camFar = 0.0f;
		};

		DepthTarget GetDepthTarget()
		{
			DepthTarget target;
			auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
			if (!renderer)
				return target;
			const auto& ds = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGET_DEPTHSTENCIL::kMAIN];
			target.dsv = ds.views[0];
			target.srv = ds.depthSRV;
			if (ds.texture) {
				D3D11_TEXTURE2D_DESC desc{};
				ds.texture->GetDesc(&desc);
				target.width = desc.Width;
				target.height = desc.Height;
			}
			static REL::Relocation<std::uintptr_t> cameraData{ REL::RelocationID(517032, 403540) };
			target.camNear = *reinterpret_cast<const float*>(cameraData.address() + 0x40);
			target.camFar = *reinterpret_cast<const float*>(cameraData.address() + 0x44);
			return target;
		}

		class PredicationRestore final
		{
		public:
			explicit PredicationRestore(ID3D11DeviceContext* context) :
				m_context(context)
			{
				BOOL value = FALSE;
				m_context->GetPredication(m_predicate.put(), &value);
				m_value = value;
			}

			~PredicationRestore() noexcept
			{
				m_context->SetPredication(m_predicate.get(), m_value);
			}

			PredicationRestore(const PredicationRestore&) = delete;
			PredicationRestore& operator=(const PredicationRestore&) = delete;

		private:
			ID3D11DeviceContext* m_context;
			winrt::com_ptr<ID3D11Predicate> m_predicate;
			BOOL m_value = FALSE;
		};

		class StencilRefRestore final
		{
		public:
			explicit StencilRefRestore(ID3D11DeviceContext* context) :
				m_context(context)
			{
				m_context->OMGetDepthStencilState(nullptr, &m_reference);
			}

			~StencilRefRestore() noexcept
			{
				winrt::com_ptr<ID3D11DepthStencilState> current;
				m_context->OMGetDepthStencilState(current.put(), nullptr);
				m_context->OMSetDepthStencilState(current.get(), m_reference);
			}

			StencilRefRestore(const StencilRefRestore&) = delete;
			StencilRefRestore& operator=(const StencilRefRestore&) = delete;

		private:
			ID3D11DeviceContext* m_context;
			UINT m_reference = 0;
		};

		struct LayerDraw
		{
			winrt::com_ptr<ID3D11Texture2D> texture;
			Matrix wvp[2]{};
			float planeDepth = 0.0f;
		};

		BackendStatus BuildDraws(const Overlay::Settings& s, float gameToMetre,
			float camNear, float camFar, HUDOcclusion::DepthConvention convention,
			std::vector<LayerDraw>& draws)
		{
			const float hudDepth = std::max(Overlay::Config::kMinGlobalHUDDepthMetres, s.hudDepth);
			const float coverage = std::clamp(s.hudCoverage,
				Overlay::Config::kMinHUDCoverage, Overlay::Config::kMaxHUDCoverage);

			auto hudClients = HelperImpl::GetSingleton().SnapshotHUDClients();
			if (hudClients.empty())
				return BackendStatus::NoLayers;

			std::vector<LayerClaim> claims;
			claims.reserve(hudClients.size());
			for (const auto& hud : hudClients) {
				if (!hud.texture)
					continue;
				const auto plane = InSceneOverlay::ResolveHUDPlane({ hud.depth, hud.coverage }, { hudDepth, coverage });
				claims.push_back({ hud.client_id, plane.depth, plane.coverage });
			}
			if (claims.empty())
				return BackendStatus::NoLayers;
			SortFarthestFirst(claims);

			for (const auto& claim : claims) {
				LayerDraw draw;
				draw.planeDepth = 0.0f;
				if (!HUDOcclusion::DepthForMetres(claim.depth, camNear, camFar, gameToMetre,
						convention, draw.planeDepth, HUDOcclusion::NearPlanePolicy::Clamp))
					continue;

				bool posed = true;
				posed = posed && InSceneOverlay::HUDPlaneWVP(vr::Eye_Left, claim.depth, claim.coverage, draw.wvp[0]);
				posed = posed && InSceneOverlay::HUDPlaneWVP(vr::Eye_Right, claim.depth, claim.coverage, draw.wvp[1]);
				if (!posed)
					return BackendStatus::NoLayers;

				for (const auto& hud : hudClients) {
					if (hud.client_id == claim.clientId) {
						draw.texture = hud.texture;
						break;
					}
				}
				if (draw.texture)
					draws.push_back(std::move(draw));
			}
			return draws.empty() ? BackendStatus::NoLayers : BackendStatus::Ready;
		}

		void FillCB(ID3D11DeviceContext* ctx, ID3D11Buffer* buffer, const void* data, std::size_t size,
			BackendStatus& status)
		{
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(ctx->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				status = BackendStatus::NoDevice;
				return;
			}
			std::memcpy(mapped.pData, data, size);
			ctx->Unmap(buffer, 0);
		}

		void LogStatusOnce(BackendStatus status)
		{
			static std::atomic<uint32_t> seen{ 0 };
			const uint32_t bit = 1u << static_cast<uint32_t>(status);
			if (seen.fetch_or(bit) & bit)
				return;
			logs::info("HUDOcclusion: {}", BackendStatusToString(status));
		}

		void StampWorldDepth()
		{
			const auto& settings = Overlay::State::GetSingleton().settings;
			const DepthTarget range = GetDepthTarget();
			HUDOcclusion::DepthConvention convention = HUDOcclusion::DepthConvention::Standard;
			const BackendStatus preconditions = [&] {
				if (!settings.hudDepthOcclusion)
					return BackendStatus::Disabled;
				if (InSceneOverlay::IsRenderPathDisabled())
					return BackendStatus::Disabled;
				if (!DrawsToMainDepth())
					return BackendStatus::NotMainDepth;
				if (!ResolveConvention(range.camNear, range.camFar, convention))
					return BackendStatus::UnusableDepth;
				return BackendStatus::Ready;
			}();

			if (!IsReady(preconditions)) {
				g_status.store(preconditions, std::memory_order_relaxed);
				LogStatusOnce(preconditions);
				return;
			}

			auto* device = Globals::GetD3D().device;
			auto* ctx = Globals::GetD3D().context;
			if (!device || !ctx || ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
				g_status.store(BackendStatus::NoDevice, std::memory_order_relaxed);
				return;
			}

			if (g_attemptedDevice.get() != device) {
				g_pipeline = Pipeline{};
				g_attemptedDevice.copy_from(device);
				g_pipelineFailed.store(false, std::memory_order_relaxed);
			}
			if (g_pipelineFailed.load(std::memory_order_relaxed)) {
				g_status.store(BackendStatus::NoDevice, std::memory_order_relaxed);
				return;
			}
			if (!g_pipeline.vs) {
				Pipeline pipeline;
				if (!CreatePipeline(device, pipeline)) {
					logs::error("HUDOcclusion: pipeline creation failed; world occlusion stays off");
					g_pipelineFailed.store(true, std::memory_order_relaxed);
					g_status.store(BackendStatus::NoDevice, std::memory_order_relaxed);
					return;
				}
				g_pipeline = std::move(pipeline);
			}

			const DepthTarget& target = range;
			if (!target.dsv || target.width < 2 || target.height == 0) {
				g_status.store(BackendStatus::NoDevice, std::memory_order_relaxed);
				return;
			}

			std::vector<LayerDraw> draws;
			const BackendStatus built = BuildDraws(settings, InSceneOverlay::GameUnitsToMetres(),
				target.camNear, target.camFar, convention, draws);
			if (!IsReady(built)) {
				g_status.store(built, std::memory_order_relaxed);
				LogStatusOnce(built);
				return;
			}

			auto* gameState = RE::BSGraphics::State::GetSingleton();
			if (!gameState) {
				g_status.store(BackendStatus::NoDevice, std::memory_order_relaxed);
				return;
			}
			const auto& state = gameState->GetRuntimeData();
			float drsW = 1.0f;
			float drsH = 1.0f;
			if (gameState->screenWidth == target.width && gameState->screenHeight == target.height) {
				drsW = state.dynamicResolutionWidthRatio;
				drsH = state.dynamicResolutionHeightRatio;
			}
			EyeViewport eyes[2];
			if (!MakeEyeViewport(target.width, target.height, drsW, drsH, false, eyes[0]) ||
				!MakeEyeViewport(target.width, target.height, drsW, drsH, true, eyes[1])) {
				g_status.store(BackendStatus::UnusableDepth, std::memory_order_relaxed);
				return;
			}

			// Restore after the guard so its ClearState cannot overwrite these values.
			PredicationRestore predication(ctx);
			StencilRefRestore stencilRef(ctx);

			Util::D3D11StateGuard guard(ctx);
			if (!guard.IsValid()) {
				g_status.store(BackendStatus::NoDevice, std::memory_order_relaxed);
				return;
			}

			ctx->OMSetRenderTargets(0, nullptr, target.dsv);
			ctx->OMSetBlendState(g_pipeline.noColor.get(), nullptr, 0xFFFFFFFF);
			ctx->OMSetDepthStencilState(g_pipeline.depthStencilWrite.get(), kHudStencilRef);
			ctx->RSSetState(g_pipeline.rasterizer.get());
			ctx->IASetInputLayout(g_pipeline.layout.get());
			ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			const UINT stride = sizeof(DirectX::XMFLOAT3) + sizeof(DirectX::XMFLOAT2);
			const UINT offset = 0;
			ID3D11Buffer* vb = g_pipeline.vb.get();
			ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
			ctx->IASetIndexBuffer(g_pipeline.ib.get(), DXGI_FORMAT_R32_UINT, 0);
			ctx->VSSetShader(g_pipeline.vs.get(), nullptr, 0);
			ctx->PSSetShader(g_pipeline.ps.get(), nullptr, 0);
			ID3D11Buffer* quadCB = g_pipeline.quadCB.get();
			ctx->VSSetConstantBuffers(0, 1, &quadCB);
			ID3D11Buffer* depthCB = g_pipeline.depthCB.get();
			ctx->PSSetConstantBuffers(1, 1, &depthCB);
			ID3D11SamplerState* sampler = g_pipeline.sampler.get();
			ctx->PSSetSamplers(0, 1, &sampler);

			BackendStatus status = BackendStatus::Ready;
			for (const auto& draw : draws) {
				winrt::com_ptr<ID3D11ShaderResourceView> layerSRV;
				if (FAILED(device->CreateShaderResourceView(draw.texture.get(), nullptr, layerSRV.put()))) {
					logs::warn("HUDOcclusion: layer SRV creation failed; skipping this frame");
					status = BackendStatus::NoDevice;
					break;
				}

				DepthCB depthCBData;
				depthCBData.planeDepth = draw.planeDepth;
				FillCB(ctx, g_pipeline.depthCB.get(), &depthCBData, sizeof(depthCBData), status);
				if (!IsReady(status))
					break;

				ID3D11ShaderResourceView* srv = layerSRV.get();
				ctx->PSSetShaderResources(0, 1, &srv);
				for (int eye = 0; eye < 2; ++eye) {
					QuadCB quadCBData;
					quadCBData.wvp = draw.wvp[eye];
					FillCB(ctx, g_pipeline.quadCB.get(), &quadCBData, sizeof(quadCBData), status);
					if (!IsReady(status))
						break;
					const D3D11_VIEWPORT viewport{ eyes[eye].x, eyes[eye].y,
						eyes[eye].width, eyes[eye].height, 0.0f, 1.0f };
					ctx->RSSetViewports(1, &viewport);
					ctx->DrawIndexed(6, 0, 0);
				}
				ID3D11ShaderResourceView* none = nullptr;
				ctx->PSSetShaderResources(0, 1, &none);
				if (!IsReady(status))
					break;
			}

			g_status.store(status, std::memory_order_relaxed);
			if (IsReady(status))
				LogStatusOnce(status);
		}

		struct RenderWorldStart
		{
			static void thunk(RE::BSBatchRenderer* renderer, uint32_t startRange,
				uint32_t endRange, uint32_t flags, int geometryGroup)
			{
				StampWorldDepth();
				func(renderer, startRange, endRange, flags, geometryGroup);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

	}

	void Install()
	{
		if (!REL::Module::IsVR())
			return;
		if (g_installed.exchange(true))
			return;
		if (!Globals::IsReady()) {
			g_installed.store(false);
			return;
		}

		constexpr std::size_t kThunkSlotSize = 14;
		if (SKSE::GetTrampoline().free_size() < kThunkSlotSize) {
			logs::error(
				"HUDOcclusion: no trampoline room ({} bytes free); world occlusion "
				"stays off and the world renders normally",
				SKSE::GetTrampoline().free_size());
			g_installed.store(false);
			return;
		}

		try {
			stl::write_thunk_call<RenderWorldStart>(
				REL::RelocationID(99938, 106583).address() + REL::Relocate(0x8E, 0x84));
			logs::info("HUDOcclusion: world-start hook installed ({}); {}",
				reinterpret_cast<void*>(RenderWorldStart::func.address()), StatusText());
		} catch (const std::exception& e) {
			logs::error("HUDOcclusion: hook install failed: {}", e.what());
			g_installed.store(false);
		}
	}

	BackendStatus LastStatus()
	{
		return g_status.load(std::memory_order_relaxed);
	}

	const char* StatusText()
	{
		return BackendStatusToString(LastStatus());
	}
}
