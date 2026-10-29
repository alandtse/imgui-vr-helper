// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Depth-only world occlusion behind fully opaque HUD pixels.
//
// Pixels the HUD covers completely are invisible behind it, so rasterising the
// world there is wasted work. Before the game issues its first world batch this
// stamps each HUD plane's depth into kMAIN wherever that layer's own canvas alpha
// is opaque, so the depth test discards the world behind the panel while
// anything nearer still draws.
//
// Opacity comes from the layer's own canvas, never from a separate client mask,
// so the write cannot drift from the art composited on top of it. Colour and
// stencil are never written. Every situation this code cannot prove safe renders
// the world normally instead of guessing.
//
// Known limitation: layer canvases are filled during DispatchFrame while the
// world renders between presents, so a canvas that changes from fully opaque to
// transparent can leave the world missing behind that area for a single frame.
// The 254/255 threshold keeps this to opaque<->transparent transitions, and the
// setting is off by default because only a headset can confirm it looks right.

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
		// Same unit-quad geometry and constant layout the HUD pass uses, so the
		// depth write lands on exactly the pixels the HUD composites over. The
		// pixel shader writes only SV_Depth, and only where the layer's own canvas
		// alpha is opaque — blended art keeps the world visible through it.
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
			// The tested constant, not a magic literal duplicated in HLSL.
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
			winrt::com_ptr<ID3D11RasterizerState> rasterizer;
			winrt::com_ptr<ID3D11BlendState> noColor;
		};

		Pipeline g_pipeline;
		std::atomic<bool> g_installed{ false };
		std::atomic<BackendStatus> g_status{ BackendStatus::Disabled };
		std::atomic<bool> g_conflictLogged{ false };
		std::atomic<bool> g_pipelineFailed{ false };
		/// Device the cached pipeline was created on; a different device means the
		/// cached state objects belong to something else and must not be bound.
		std::atomic<ID3D11Device*> g_pipelineDevice{ nullptr };

		const char* BackendStatusToString(BackendStatus status)
		{
			switch (status) {
			case BackendStatus::Ready:
				return "active";
			case BackendStatus::Disabled:
				return "off";
			case BackendStatus::NoLayers:
				return "idle (no HUD layer this frame)";
			case BackendStatus::RendererConflict:
				return "unavailable: another renderer mod owns the depth buffer";
			case BackendStatus::NotMainDepth:
				return "unavailable: not drawing the main depth target";
			case BackendStatus::UnusableDepth:
				return "unavailable: camera depth range unusable";
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

		bool CreatePipeline(ID3D11Device* device)
		{
			const auto vsBlob = Compile(kVertexShader, "vs_5_0", "vertex");
			const auto psBlob = Compile(kPixelShader, "ps_5_0", "pixel");
			if (!vsBlob || !psBlob)
				return false;
			if (FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
					nullptr, g_pipeline.vs.put())) ||
				FAILED(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
					nullptr, g_pipeline.ps.put()))) {
				logs::error("HUDOcclusion: shader creation failed");
				return false;
			}

			const D3D11_INPUT_ELEMENT_DESC elems[] = {
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			};
			if (FAILED(device->CreateInputLayout(elems, 2, vsBlob->GetBufferPointer(),
					vsBlob->GetBufferSize(), g_pipeline.layout.put()))) {
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
			if (FAILED(device->CreateBuffer(&cbDesc, nullptr, g_pipeline.quadCB.put())))
				return false;
			cbDesc.ByteWidth = sizeof(DepthCB);
			if (FAILED(device->CreateBuffer(&cbDesc, nullptr, g_pipeline.depthCB.put())))
				return false;
			if (FAILED(device->CreateBuffer(&vbDesc, &vbData, g_pipeline.vb.put())) ||
				FAILED(device->CreateBuffer(&ibDesc, &ibData, g_pipeline.ib.put()))) {
				logs::error("HUDOcclusion: geometry buffer creation failed");
				return false;
			}

			D3D11_SAMPLER_DESC samplerDesc{};
			// Bilinear, matching the composite's sampler, so a pixel the composite
			// renders as partly transparent is not point-sampled as fully opaque.
			samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
			if (FAILED(device->CreateSamplerState(&samplerDesc, g_pipeline.sampler.put())))
				return false;

			// Depth writes only: no stencil, no colour. Stencil is left untouched
			// because other VR machinery reads the values the game wrote.
			D3D11_DEPTH_STENCIL_DESC dsDesc{};
			dsDesc.DepthEnable = TRUE;
			dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
			dsDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
			dsDesc.StencilEnable = FALSE;
			if (FAILED(device->CreateDepthStencilState(&dsDesc, g_pipeline.depthWrite.put())))
				return false;

			D3D11_RASTERIZER_DESC rasDesc{};
			rasDesc.FillMode = D3D11_FILL_SOLID;
			rasDesc.CullMode = D3D11_CULL_NONE;
			rasDesc.DepthClipEnable = FALSE;  // the plane can sit outside the clip range
			if (FAILED(device->CreateRasterizerState(&rasDesc, g_pipeline.rasterizer.put())))
				return false;

			D3D11_BLEND_DESC blendDesc{};
			blendDesc.RenderTarget[0].RenderTargetWriteMask = 0;
			if (FAILED(device->CreateBlendState(&blendDesc, g_pipeline.noColor.put())))
				return false;

			return true;
		}

		// ---- Engine state probes ----------------------------------------

		/// True only when the engine camera positively looks like a standard-Z
		/// (near = 0) perspective projection, which is the only case a standard-Z
		/// value can be written into. A renderer mod may run depth reversed
		/// (near = 1), so an unreadable or unfamiliar projection must not stamp:
		/// guessing wrong culls geometry that should have been visible.
		/// True when the engine camera positively looks like a standard-Z (near = 0)
		/// perspective projection, which is the only thing a standard-Z depth value
		/// may be written into. A renderer mod can run depth reversed (near = 1), so
		/// an unreadable or unfamiliar projection must not stamp: guessing wrong
		/// culls geometry that should have stayed visible.
		///
		/// `camNear`/`camFar` are cross-checked against the same matrix here rather
		/// than trusted on their own. They come from the camera data block (the same
		/// source the shipped world-quad depth compare uses), while the matrix is
		/// what proves the depth convention; agreeing on the ratio means both refer
		/// to the frustum actually being rendered.
		bool StandardDepthConfirmed(float camNear, float camFar)
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
			if (std::abs(m[15]) > 1e-4f)
				return false;  // not a perspective projection
			const bool column = std::abs(std::abs(m[14]) - 1.0f) < 1e-3f;
			const bool row = std::abs(std::abs(m[11]) - 1.0f) < 1e-3f;
			if (column == row)
				return false;  // neither layout, or both: unrecognised
			const float perspective = column ? m[14] : m[11];
			if (!(m[10] * perspective > 0.0f))
				return false;  // reversed or degenerate depth

			// Plausible range, then agree with the matrix. For a standard-Z frustum
			// the depth scale is far/(far-near), so a range that does not reproduce
			// it is not describing this projection.
			if (!(camNear > 0.0f) || !(camFar > camNear))
				return false;
			const float ratio = camFar / camNear;
			if (!(ratio > 1.0f) || ratio > 1.0e6f)
				return false;
			const float expectedScale = camFar / (camFar - camNear);
			const float actualScale = std::abs(m[10]);
			return std::abs(actualScale - expectedScale) <= 0.05f * expectedScale;
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

		/// Predication is context state but NOT part of a device-context state
		/// object, so D3D11StateGuard's ClearState/swap would silently drop it and
		/// leave the rest of the game's frame unpredicated. Construct this BEFORE
		/// the guard so it restores after the guard has reinstated everything else.
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

		// ---- Draw ------------------------------------------------------

		/// One layer's quad: the same HUD projection the composite uses, per eye,
		/// plus the standard-Z depth of that layer's plane.
		struct LayerDraw
		{
			// Held, not borrowed: the snapshot that produced it is a local, so a raw
			// pointer could dangle if a client unregisters before the draw.
			winrt::com_ptr<ID3D11Texture2D> texture;
			Matrix wvp[2]{};
			float planeDepth = 0.0f;
		};

		BackendStatus BuildDraws(const Overlay::Settings& s, float gameToMetre,
			float camNear, float camFar, std::vector<LayerDraw>& draws)
		{
			const float hudDepth = std::max(0.3f, s.hudDepth);
			const float coverage = std::clamp(s.hudCoverage,
				Overlay::Config::kMinHUDCoverage, Overlay::Config::kMaxHUDCoverage);

			auto hudClients = HelperImpl::GetSingleton().SnapshotHUDClients();
			if (hudClients.empty())
				return BackendStatus::NoLayers;

			// Farthest first: each layer writes depth only where its own canvas is
			// opaque, so the nearest opaque layer wins a shared pixel.
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
				if (!StandardZDepth(claim.depth, camNear, camFar, gameToMetre, draw.planeDepth))
					return BackendStatus::UnusableDepth;

				bool posed = true;
				posed = posed && InSceneOverlay::HUDPlaneWVP(vr::Eye_Left, claim.depth, claim.coverage, draw.wvp[0]);
				posed = posed && InSceneOverlay::HUDPlaneWVP(vr::Eye_Right, claim.depth, claim.coverage, draw.wvp[1]);
				if (!posed)
					return BackendStatus::NoLayers;  // pose/projection cache not ready yet

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

		/// Community Shaders (and its Open Shaders successor) owns depth semantics:
		/// it rewrites depth-stencil state on submit and consumes exact near-plane
		/// depth for its HMD mask, so a stamp here is not provably safe. Re-checked
		/// every frame rather than latched, since the module can load after us.
		bool RendererOwnsDepth()
		{
			if (GetModuleHandleW(L"CommunityShaders.dll"))
				return true;
			return false;
		}

		void StampWorldDepth()
		{
			const auto& settings = Overlay::State::GetSingleton().settings;
			const DepthTarget range = GetDepthTarget();
			const BackendStatus preconditions = [&] {
				if (!settings.hudDepthOcclusion)
					return BackendStatus::Disabled;
				if (InSceneOverlay::IsRenderPathDisabled())
					return BackendStatus::Disabled;
				if (RendererOwnsDepth()) {
					if (!g_conflictLogged.exchange(true))
						logs::info("HUDOcclusion: Community Shaders owns the depth buffer; world occlusion stays off");
					return BackendStatus::RendererConflict;
				}
				if (!DrawsToMainDepth())
					return BackendStatus::NotMainDepth;
				if (!StandardDepthConfirmed(range.camNear, range.camFar))
					return BackendStatus::UnusableDepth;
				return BackendStatus::Ready;
			}();

			if (!IsReady(preconditions)) {
				g_status.store(preconditions, std::memory_order_relaxed);
				return;
			}

			auto* device = Globals::GetD3D().device;
			auto* ctx = Globals::GetD3D().context;
			if (!device || !ctx || ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
				g_status.store(BackendStatus::NoDevice, std::memory_order_relaxed);
				return;
			}

			if (!g_pipeline.vs || g_pipelineDevice.load(std::memory_order_relaxed) != device) {
				if (g_pipeline.vs && g_pipelineDevice.load(std::memory_order_relaxed) != device) {
					// A different device: drop the old pipeline rather than bind its
					// state objects to a device that never created them.
					g_pipeline = Pipeline{};
					g_pipelineFailed.store(false, std::memory_order_relaxed);
				}
				if (g_pipelineFailed.load(std::memory_order_relaxed)) {
					g_status.store(BackendStatus::NoDevice, std::memory_order_relaxed);
					return;
				}
				if (!CreatePipeline(device)) {
					logs::error("HUDOcclusion: pipeline creation failed; world occlusion stays off");
					g_pipelineFailed.store(true, std::memory_order_relaxed);
					g_status.store(BackendStatus::NoDevice, std::memory_order_relaxed);
					return;
				}
				g_pipelineDevice.store(device, std::memory_order_relaxed);
			}

			const DepthTarget& target = range;
			if (!target.dsv || target.width < 2 || target.height == 0) {
				g_status.store(BackendStatus::NoDevice, std::memory_order_relaxed);
				return;
			}

			// The same game-unit-to-metre conversion (room scale included) the
			// world-quad depth compare uses, so the plane's depth and the geometry it
			// occludes share one scale and cannot drift apart.
			std::vector<LayerDraw> draws;
			const BackendStatus built = BuildDraws(settings, InSceneOverlay::GameUnitsToMetres(),
				target.camNear, target.camFar, draws);
			if (!IsReady(built)) {
				g_status.store(built, std::memory_order_relaxed);
				return;
			}

			// Dynamic resolution renders a sub-rect of a full-size target, but some
			// configurations shrink the depth texture itself. Applying the ratio to an
			// already-shrunk target would shrivel the stamp, so only apply it when the
			// target still matches the screen size.
			// screenWidth/Height are direct State members; the ratios live in the
			// runtime block, so both views of the same object are needed.
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

			// Declared before the guard so it restores after the guard has put the
			// pipeline back: predication is not part of a device-context state.
			PredicationRestore predication(ctx);

			// Everything else this draw touches is restored on scope exit, including
			// stage slots a plain save/restore would miss.
			Util::D3D11StateGuard guard(ctx);
			if (!guard.IsValid()) {
				g_status.store(BackendStatus::NoDevice, std::memory_order_relaxed);
				return;
			}

			ctx->OMSetRenderTargets(0, nullptr, target.dsv);
			ctx->OMSetBlendState(g_pipeline.noColor.get(), nullptr, 0xFFFFFFFF);
			ctx->OMSetDepthStencilState(g_pipeline.depthWrite.get(), 0);
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
		}

		// ---- Hook ------------------------------------------------------

		struct RenderWorldStart
		{
			static void thunk(RE::BSBatchRenderer* renderer, uint32_t startRange,
				uint32_t endRange, uint32_t flags, int geometryGroup)
			{
				// Runs before the first world batch, so the depth test rejects the
				// geometry the HUD is about to cover.
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

		// The same call site other world-render mods use. The trampoline chains
		// onto whatever was installed before, so an existing owner keeps running
		// (after this stamp for our hook, since SKSE chains newest-first).
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
