// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Lifted from skyrim-community-shaders/src/Features/VR/InSceneOverlay.cpp
// with relicensing under GPL-3.0-or-later WITH the modding exception.
//
// Adapted: shaders are embedded as inline string literals (compiled with
// D3DCompile at init) so the helper has no on-disk shader file dependency.
// SCS-specific bits dropped: ApplyHighlightTintToTexture, IsWelcomeOverlayVisible,
// SetResourceName. menu texture source is the focused client's panel.

#include "pch.h"

#include "InSceneOverlay.h"

#include "Globals.h"
#include "HelperImpl.h"
#include "Overlay.h"
#include "OverlayTinter.h"
#include "internal/Detour.h"
#include "internal/HUDGeometry.h"
#include "internal/VRUtils.h"

#include <RE/B/BSOpenVR.h>

#include <DirectXMath.h>
#include <SimpleMath.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>

#include <atomic>
#include <exception>

#pragma comment(lib, "d3dcompiler.lib")

namespace ImGuiVRHelper::InSceneOverlay
{
	using DirectX::XMFLOAT2;
	using DirectX::XMFLOAT3;
	using DirectX::SimpleMath::Matrix;
	using DirectX::SimpleMath::Vector3;

	namespace
	{
		// ---- Inline shader sources --------------------------------------
		// Pulled in source-form from
		// skyrim-community-shaders/package/Shaders/VR/InSceneOverlay.{vs,ps}.hlsl
		// so the helper has no on-disk shader file dependency.

		// Tint blending lives in OverlayTinter's compute shader pass (run
		// once per frame on the focused client's panel before composite).
		// Composite shaders here are pure pass-through.

		constexpr const char* kVertexShader = R"(
cbuffer MatrixBuffer : register(b0)
{
	matrix wvp;
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
	output.uv = input.uv;
	return output;
}
)";

		constexpr const char* kPixelShader = R"(
Texture2D shaderTexture : register(t0);
SamplerState sampleType : register(s0);
struct PS_INPUT
{
	float4 pos: SV_POSITION;
	float2 uv: TEXCOORD0;
};
float4 main(PS_INPUT input) : SV_TARGET
{
	return shaderTexture.Sample(sampleType, input.uv);
}
)";

		struct ConstantBufferData
		{
			Matrix wvp;
		};

		struct Resources
		{
			winrt::com_ptr<ID3D11VertexShader> vs;
			winrt::com_ptr<ID3D11PixelShader> ps;
			winrt::com_ptr<ID3D11InputLayout> inputLayout;
			winrt::com_ptr<ID3D11Buffer> vb;
			winrt::com_ptr<ID3D11Buffer> ib;
			winrt::com_ptr<ID3D11Buffer> cb;
			winrt::com_ptr<ID3D11BlendState> blendState;
			winrt::com_ptr<ID3D11DepthStencilState> depthState;
			winrt::com_ptr<ID3D11RasterizerState> rasterizerState;
			winrt::com_ptr<ID3D11SamplerState> sampler;

			// Cached per-eye RTVs keyed by target texture pointer (rebuilt
			// when SteamVR rotates eye textures).
			struct CachedRTV
			{
				winrt::com_ptr<ID3D11RenderTargetView> rtv;
				ID3D11Texture2D* texture = nullptr;
			};
			CachedRTV cachedEyeRTVs[2];

			// Cached SRV for the menu texture — rebuilt when the focused
			// client changes (different panel texture).
			winrt::com_ptr<ID3D11ShaderResourceView> menuSRV;
			ID3D11Texture2D* cachedMenuTexture = nullptr;

			bool initialized = false;
		};

		Resources g_res;

		// ---- Resource init ----------------------------------------------

		bool InitResources()
		{
			if (g_res.initialized)
				return true;
			if (!Globals::IsReady())
				return false;

			auto& d3d = Globals::GetD3D();
			auto* device = d3d.device;

			// Compile shaders from embedded strings.
			winrt::com_ptr<ID3DBlob> vsBlob, psBlob, errorBlob;
			HRESULT hr = D3DCompile(
				kVertexShader, std::strlen(kVertexShader), "InSceneOverlay.vs", nullptr, nullptr,
				"main", "vs_5_0",
				D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
				vsBlob.put(), errorBlob.put());
			if (FAILED(hr)) {
				logs::error("InSceneOverlay VS compile failed: {}",
					errorBlob ? static_cast<const char*>(errorBlob->GetBufferPointer()) : "<no message>");
				return false;
			}
			errorBlob = nullptr;
			hr = D3DCompile(
				kPixelShader, std::strlen(kPixelShader), "InSceneOverlay.ps", nullptr, nullptr,
				"main", "ps_5_0",
				D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
				psBlob.put(), errorBlob.put());
			if (FAILED(hr)) {
				logs::error("InSceneOverlay PS compile failed: {}",
					errorBlob ? static_cast<const char*>(errorBlob->GetBufferPointer()) : "<no message>");
				return false;
			}

			if (FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
					nullptr, g_res.vs.put()))) {
				logs::error("InSceneOverlay: CreateVertexShader failed");
				return false;
			}
			if (FAILED(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
					nullptr, g_res.ps.put()))) {
				logs::error("InSceneOverlay: CreatePixelShader failed");
				return false;
			}

			// Input layout.
			D3D11_INPUT_ELEMENT_DESC layout[2] = {
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
					D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT,
					D3D11_INPUT_PER_VERTEX_DATA, 0 },
			};
			if (FAILED(device->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(),
					vsBlob->GetBufferSize(), g_res.inputLayout.put()))) {
				logs::error("InSceneOverlay: CreateInputLayout failed");
				return false;
			}

			// Quad vertices (XY plane, z=0, size=1).
			struct VertexType
			{
				XMFLOAT3 position;
				XMFLOAT2 texture;
			};
			VertexType vertices[4] = {
				{ { -0.5f, -0.5f, 0.0f }, { 0.0f, 1.0f } },  // Bottom Left
				{ { -0.5f, 0.5f, 0.0f }, { 0.0f, 0.0f } },   // Top Left
				{ { 0.5f, 0.5f, 0.0f }, { 1.0f, 0.0f } },    // Top Right
				{ { 0.5f, -0.5f, 0.0f }, { 1.0f, 1.0f } },   // Bottom Right
			};

			D3D11_BUFFER_DESC vbDesc = {};
			vbDesc.Usage = D3D11_USAGE_DEFAULT;
			vbDesc.ByteWidth = sizeof(vertices);
			vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
			D3D11_SUBRESOURCE_DATA vbData = { vertices };
			if (FAILED(device->CreateBuffer(&vbDesc, &vbData, g_res.vb.put()))) {
				logs::error("InSceneOverlay: vertex buffer creation failed");
				return false;
			}

			uint32_t indices[6] = { 0, 1, 2, 0, 2, 3 };
			D3D11_BUFFER_DESC ibDesc = {};
			ibDesc.Usage = D3D11_USAGE_DEFAULT;
			ibDesc.ByteWidth = sizeof(indices);
			ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
			D3D11_SUBRESOURCE_DATA ibData = { indices };
			if (FAILED(device->CreateBuffer(&ibDesc, &ibData, g_res.ib.put()))) {
				logs::error("InSceneOverlay: index buffer creation failed");
				return false;
			}

			D3D11_BUFFER_DESC cbDesc = {};
			cbDesc.Usage = D3D11_USAGE_DYNAMIC;
			cbDesc.ByteWidth = sizeof(ConstantBufferData);
			cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			if (FAILED(device->CreateBuffer(&cbDesc, nullptr, g_res.cb.put()))) {
				logs::error("InSceneOverlay: constant buffer creation failed");
				return false;
			}

			// Blend state: standard alpha blending so the menu doesn't paint
			// black over the eye render where it's transparent.
			D3D11_BLEND_DESC blendDesc = {};
			blendDesc.RenderTarget[0].BlendEnable = TRUE;
			blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
			blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
			blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
			blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			blendDesc.RenderTarget[0].RenderTargetWriteMask = 0x0F;
			if (FAILED(device->CreateBlendState(&blendDesc, g_res.blendState.put()))) {
				logs::error("InSceneOverlay: blend state creation failed");
				return false;
			}

			// Depth always pass, no write — overlay always on top.
			D3D11_DEPTH_STENCIL_DESC depthDesc = {};
			depthDesc.DepthEnable = FALSE;
			depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			depthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
			if (FAILED(device->CreateDepthStencilState(&depthDesc, g_res.depthState.put()))) {
				logs::error("InSceneOverlay: depth stencil state creation failed");
				return false;
			}

			D3D11_RASTERIZER_DESC rasterDesc = {};
			rasterDesc.FillMode = D3D11_FILL_SOLID;
			rasterDesc.CullMode = D3D11_CULL_NONE;
			rasterDesc.FrontCounterClockwise = FALSE;
			rasterDesc.DepthClipEnable = TRUE;
			if (FAILED(device->CreateRasterizerState(&rasterDesc, g_res.rasterizerState.put()))) {
				logs::error("InSceneOverlay: rasterizer state creation failed");
				return false;
			}

			D3D11_SAMPLER_DESC samplerDesc = {};
			samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
			samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
			samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
			samplerDesc.MinLOD = 0;
			samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
			if (FAILED(device->CreateSamplerState(&samplerDesc, g_res.sampler.put()))) {
				logs::error("InSceneOverlay: sampler creation failed");
				return false;
			}

			g_res.initialized = true;
			logs::info("InSceneOverlay resources initialized");
			return true;
		}

		// ---- Cached RTV/SRV helpers -------------------------------------

		ID3D11RenderTargetView* GetEyeRTV(vr::EVREye eye, ID3D11Texture2D* tex)
		{
			const int eyeIdx = static_cast<int>(eye);
			auto& cached = g_res.cachedEyeRTVs[eyeIdx];
			if (cached.texture == tex && cached.rtv)
				return cached.rtv.get();

			cached.rtv = nullptr;
			cached.texture = nullptr;

			D3D11_TEXTURE2D_DESC texDesc;
			tex->GetDesc(&texDesc);

			D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
			rtvDesc.Format = texDesc.Format;
			if (texDesc.ArraySize > 1) {
				if (texDesc.SampleDesc.Count > 1) {
					rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
					rtvDesc.Texture2DMSArray.FirstArraySlice = static_cast<UINT>(eye);
					rtvDesc.Texture2DMSArray.ArraySize = 1;
				} else {
					rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
					rtvDesc.Texture2DArray.FirstArraySlice = static_cast<UINT>(eye);
					rtvDesc.Texture2DArray.ArraySize = 1;
					rtvDesc.Texture2DArray.MipSlice = 0;
				}
			} else if (texDesc.SampleDesc.Count > 1) {
				rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
			} else {
				rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
				rtvDesc.Texture2D.MipSlice = 0;
			}

			if (FAILED(Globals::GetD3D().device->CreateRenderTargetView(tex, &rtvDesc, cached.rtv.put()))) {
				logs::error("InSceneOverlay: failed to create eye RTV (fmt={}, samples={})",
					static_cast<uint32_t>(texDesc.Format), texDesc.SampleDesc.Count);
				return nullptr;
			}
			cached.texture = tex;
			return cached.rtv.get();
		}

		ID3D11ShaderResourceView* GetMenuSRV(ID3D11Texture2D* menuTex)
		{
			if (!menuTex)
				return nullptr;
			if (g_res.cachedMenuTexture == menuTex && g_res.menuSRV) {
				return g_res.menuSRV.get();
			}
			g_res.menuSRV = nullptr;
			if (FAILED(Globals::GetD3D().device->CreateShaderResourceView(menuTex, nullptr, g_res.menuSRV.put()))) {
				logs::error("InSceneOverlay: failed to create menu SRV");
				return nullptr;
			}
			g_res.cachedMenuTexture = menuTex;
			return g_res.menuSRV.get();
		}

		// ---- Render-path fault latch ------------------------------------

		std::atomic<bool> g_renderDisabled{ false };

		// ---- Submit hook ------------------------------------------------

		struct SubmitDetour
		{
			using FnType = vr::EVRCompositorError (*)(
				vr::IVRCompositor*, vr::EVREye, const vr::Texture_t*,
				const vr::VRTextureBounds_t*, vr::EVRSubmitFlags);

			static FnType original;

			static vr::EVRCompositorError thunk(vr::IVRCompositor* self, vr::EVREye eye,
				const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds,
				vr::EVRSubmitFlags flags)
			{
				if (!g_renderDisabled.load(std::memory_order_relaxed) &&
					texture && texture->handle && texture->eType == vr::TextureType_DirectX) {
					// Guard against vrclient throwing std::system_error
					// ("device or resource busy") under runtime contention. On
					// first fault we latch the render path off for the session
					// rather than letting the exception crash the game.
					try {
						RenderForEye(eye, static_cast<ID3D11Texture2D*>(texture->handle), bounds);
					} catch (const std::exception& e) {
						DisableRenderPath("exception in RenderForEye");
						logs::error("InSceneOverlay: render path disabled after exception: {}", e.what());
					}
				}
				return original(self, eye, texture, bounds, flags);
			}
		};

		SubmitDetour::FnType SubmitDetour::original = nullptr;

		bool g_hookInstalled = false;

		// ---- Helper: per-eye view-projection matrices -------------------

		struct EyeMatrices
		{
			Matrix vpHeadSpace;   // for HMD-relative attach mode
			Matrix vpWorldSpace;  // for controller-attached and fixed-world
			// Per-eye projection tangents (signed: left/bottom negative,
			// right/top positive). Used to size a head-locked HUD quad to
			// exactly fill this eye's (asymmetric) frustum at a given depth.
			float tanLeft = 0, tanRight = 0, tanBottom = 0, tanTop = 0;
			bool valid = false;
		};

		EyeMatrices ComputeEyeMatrices(vr::EVREye eye)
		{
			EyeMatrices result;
			auto* openvr = RE::BSOpenVR::GetSingleton();
			if (!openvr || !openvr->vrSystem)
				return result;

			vr::TrackedDevicePose_t pose[vr::k_unMaxTrackedDeviceCount];
			auto* compositor = RE::BSOpenVR::GetIVRCompositor();
			if (!compositor)
				return result;
			if (compositor->GetLastPoses(pose, vr::k_unMaxTrackedDeviceCount, nullptr, 0) !=
				vr::VRCompositorError_None) {
				return result;
			}
			const auto& hmdPose = pose[vr::k_unTrackedDeviceIndex_Hmd];
			if (!hmdPose.bPoseIsValid)
				return result;

			// Eye-to-head and projection are read from the property cache
			// (populated on the input thread) rather than queried here — calling
			// IVRSystem property methods from the render thread races
			// skyrimvrtools on vrclient's CClientPropertyManager and crashes.
			vr::HmdMatrix34_t eyeToHeadRaw;
			if (!Util::CachedEyeToHead(eye, eyeToHeadRaw))
				return result;  // cache not populated yet (very early startup)
			Matrix eyeToHead = Util::HmdMatrix34ToMatrix(eyeToHeadRaw);
			Matrix hmdWorld = Util::HmdMatrix34ToMatrix(hmdPose.mDeviceToAbsoluteTracking);

			// GetProjectionRaw returns left/right/bottom/top tangents (note
			// Valve's known parameter-name mismatch — the 3rd param is
			// actually bottom, the 4th is top).
			float left, right, bottom, top;
			if (!Util::CachedProjectionRaw(eye, left, right, bottom, top))
				return result;
			constexpr float nearZ = 0.1f;
			constexpr float farZ = 1000.0f;
			Matrix proj = DirectX::XMMatrixPerspectiveOffCenterRH(
				left * nearZ, right * nearZ, bottom * nearZ, top * nearZ, nearZ, farZ);

			result.vpHeadSpace = eyeToHead.Invert() * proj;
			Matrix eyeToWorld = eyeToHead * hmdWorld;
			result.vpWorldSpace = eyeToWorld.Invert() * proj;
			result.tanLeft = left;
			result.tanRight = right;
			result.tanBottom = bottom;
			result.tanTop = top;
			result.valid = true;
			return result;
		}

		// ---- D3D11 state save/restore -----------------------------------

		struct StateBackup
		{
			ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
			ID3D11DepthStencilView* dsv = nullptr;
			D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			UINT numViewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
			ID3D11RasterizerState* raster = nullptr;
			ID3D11BlendState* blend = nullptr;
			FLOAT blendFactor[4]{};
			UINT sampleMask = 0;
			ID3D11DepthStencilState* depth = nullptr;
			UINT stencilRef = 0;

			void Save(ID3D11DeviceContext* ctx)
			{
				ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
				ctx->RSGetViewports(&numViewports, viewports);
				ctx->RSGetState(&raster);
				ctx->OMGetBlendState(&blend, blendFactor, &sampleMask);
				ctx->OMGetDepthStencilState(&depth, &stencilRef);
			}

			void Restore(ID3D11DeviceContext* ctx)
			{
				ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
				ctx->RSSetViewports(numViewports, viewports);
				ctx->OMSetBlendState(blend, blendFactor, sampleMask);
				ctx->OMSetDepthStencilState(depth, stencilRef);
				if (raster) {
					ctx->RSSetState(raster);
					raster->Release();
				}
				if (blend)
					blend->Release();
				if (depth)
					depth->Release();
				for (auto* r : rtvs) {
					if (r)
						r->Release();
				}
				if (dsv)
					dsv->Release();
			}
		};

		// ---- Quad draw --------------------------------------------------

		void DrawQuad(ID3D11DeviceContext* ctx, const ConstantBufferData& cbData,
			ID3D11ShaderResourceView* srv)
		{
			D3D11_MAPPED_SUBRESOURCE mapped;
			if (SUCCEEDED(ctx->Map(g_res.cb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				std::memcpy(mapped.pData, &cbData, sizeof(cbData));
				ctx->Unmap(g_res.cb.get(), 0);
			}

			ctx->VSSetShader(g_res.vs.get(), nullptr, 0);
			ctx->PSSetShader(g_res.ps.get(), nullptr, 0);

			ID3D11Buffer* cb = g_res.cb.get();
			ctx->VSSetConstantBuffers(0, 1, &cb);

			struct VT
			{
				XMFLOAT3 p;
				XMFLOAT2 t;
			};
			UINT stride = sizeof(VT);
			UINT offset = 0;
			ID3D11Buffer* vb = g_res.vb.get();
			ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
			ctx->IASetIndexBuffer(g_res.ib.get(), DXGI_FORMAT_R32_UINT, 0);
			ctx->IASetInputLayout(g_res.inputLayout.get());
			ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

			ctx->OMSetBlendState(g_res.blendState.get(), nullptr, 0xFFFFFFFF);
			ctx->OMSetDepthStencilState(g_res.depthState.get(), 0);
			ctx->RSSetState(g_res.rasterizerState.get());

			ctx->PSSetShaderResources(0, 1, &srv);
			ID3D11SamplerState* sampler = g_res.sampler.get();
			ctx->PSSetSamplers(0, 1, &sampler);

			ctx->DrawIndexed(6, 0, 0);
		}
	}  // namespace

	// ---- Public entry points --------------------------------------------

	void Install()
	{
		if (g_hookInstalled)
			return;

		auto* openvr = RE::BSOpenVR::GetSingleton();
		auto* compositor = openvr ? RE::BSOpenVR::GetIVRCompositor() : nullptr;
		if (!compositor) {
			logs::warn("InSceneOverlay::Install: IVRCompositor unavailable");
			return;
		}

		// IVRCompositor::Submit is vtable slot 5 (after SetTrackingSpace,
		// GetTrackingSpace, WaitGetPoses, GetLastPoses,
		// GetLastPoseForTrackedDeviceIndex).
		SubmitDetour::original = Util::DetourClassVTable<SubmitDetour::FnType>(
			compositor, 5, &SubmitDetour::thunk);
		g_hookInstalled = true;
		logs::info("InSceneOverlay: IVRCompositor::Submit detour installed (original={})",
			reinterpret_cast<void*>(SubmitDetour::original));
	}

	bool IsRenderPathDisabled()
	{
		return g_renderDisabled.load(std::memory_order_relaxed);
	}

	void DisableRenderPath(const char* reason)
	{
		// Latch once; the first caller wins and logs the reason.
		bool expected = false;
		if (g_renderDisabled.compare_exchange_strong(expected, true,
				std::memory_order_relaxed)) {
			logs::error("InSceneOverlay: render path disabled for this session ({})",
				reason ? reason : "unspecified");
		}
	}

	namespace
	{
		// Per-client SRV cache for HUD-mode panels. Keyed by texture
		// pointer so we rebuild only when a client's RTV is reallocated.
		// The single 'menuSRV' field above only handles the focused
		// panel client; HUD clients need their own SRV slot each.
		struct HUDSRVCache
		{
			ID3D11Texture2D* texture = nullptr;
			winrt::com_ptr<ID3D11ShaderResourceView> srv;
		};
		std::unordered_map<uint32_t /*client_id*/, HUDSRVCache> g_hudSRVs;

		ID3D11ShaderResourceView* GetOrCreateHUDSRV(uint32_t client_id, ID3D11Texture2D* tex)
		{
			if (!tex)
				return nullptr;
			auto& cache = g_hudSRVs[client_id];
			if (cache.texture == tex && cache.srv)
				return cache.srv.get();
			cache.srv = nullptr;
			if (FAILED(Globals::GetD3D().device->CreateShaderResourceView(tex, nullptr, cache.srv.put()))) {
				logs::error("InSceneOverlay: failed to create HUD SRV for client_id={}", client_id);
				return nullptr;
			}
			cache.texture = tex;
			return cache.srv.get();
		}
	}

	void RenderForEye(vr::EVREye eye, ID3D11Texture2D* targetTexture,
		const vr::VRTextureBounds_t* bounds)
	{
		if (!targetTexture)
			return;

		auto& overlayState = Overlay::State::GetSingleton();
		const auto& s = overlayState.settings;

		// Two passes per eye:
		//
		// 1. HUD pass: every kClientFlag_HUDMode client gets composited as
		//    a full-viewport alpha-blended quad. Always rendered, no
		//    focus / attachMode gating — HUD content (subtitles, damage
		//    numbers, world-anchored 2D labels) is meant to be persistent.
		// 2. Panel pass: the focused (panel-mode) client gets rendered
		//    as a 3D quad in head/world/controller space, gated by the
		//    user's attachMode / positioningMethod settings.
		//
		// Either pass is allowed to be empty. If both are empty, we skip
		// the whole frame.
		const uint32_t focused = HelperImpl::GetSingleton().GetFocusedClientId();
		auto hudClients = HelperImpl::GetSingleton().SnapshotHUDClients();

		// Focused panel-mode client gate (existing semantics).
		ID3D11Texture2D* menuTex = nullptr;
		bool wantPanelPass = false;
		if (focused != 0 && s.attachMode != Overlay::AttachMode::None) {
			menuTex = HelperImpl::GetSingleton().GetClientPanelTexture(focused);
			wantPanelPass = (menuTex != nullptr);
		}

		if (!wantPanelPass && hudClients.empty())
			return;

		if (!InitResources())
			return;

		auto* ctx = Globals::GetD3D().context;
		if (!ctx)
			return;

		auto* rtv = GetEyeRTV(eye, targetTexture);
		if (!rtv)
			return;

		// While dragging, the OverlayTinter compute pass writes the
		// tinted panel into its post-process texture each Present.
		// Sample that instead so the drag highlight appears. Only
		// applies to the panel-mode pass.
		const bool dragging = overlayState.dragState.dragging && s.enableDragToReposition;
		ID3D11ShaderResourceView* panelSRV = nullptr;
		if (wantPanelPass) {
			panelSRV = dragging ? OverlayTinter::GetOutputSRV() : GetMenuSRV(menuTex);
			if (!panelSRV)
				panelSRV = GetMenuSRV(menuTex);
		}

		// Eye view-projection now needed for BOTH passes — HUD-mode
		// renders its content as a 3D quad at HMD-relative depth so
		// per-eye stereo converges (without per-eye projection, the
		// same content at the same eye-buffer pixel coords lands at
		// different physical directions through each lens and the
		// brain can't fuse the two images). Skyrim's vanilla HUD does
		// this same thing — composites onto a virtual plane at a fixed
		// distance, per-eye projection.
		EyeMatrices matrices;
		if (wantPanelPass || !hudClients.empty()) {
			matrices = ComputeEyeMatrices(eye);
			if (!matrices.valid) {
				wantPanelPass = false;
				hudClients.clear();
			}
		}

		if (!wantPanelPass && hudClients.empty())
			return;

		// Save current render state so we don't clobber Skyrim's submit.
		StateBackup backup;
		backup.Save(ctx);

		// Bind our RTV; no DSV (we don't read/write depth).
		ctx->OMSetRenderTargets(1, &rtv, nullptr);

		// Viewport: respect bounds if provided (SBS / texture array layouts).
		D3D11_TEXTURE2D_DESC texDesc;
		targetTexture->GetDesc(&texDesc);
		D3D11_VIEWPORT vp = {};
		if (bounds) {
			vp.TopLeftX = bounds->uMin * texDesc.Width;
			vp.TopLeftY = bounds->vMin * texDesc.Height;
			vp.Width = (bounds->uMax - bounds->uMin) * texDesc.Width;
			vp.Height = (bounds->vMax - bounds->vMin) * texDesc.Height;
		} else {
			vp.Width = static_cast<float>(texDesc.Width);
			vp.Height = static_cast<float>(texDesc.Height);
		}
		vp.MaxDepth = 1.0f;
		ctx->RSSetViewports(1, &vp);

		// Pass 1: HUD-mode clients. Render each as a billboarded 3D
		// quad anchored to the HMD at a fixed depth, sized large
		// enough to cover most of the user's FOV. Per-eye stereo
		// projection (matrices.vpHeadSpace) gives both eyes the
		// disparity they need to fuse the image at HUD plane depth —
		// matches what Skyrim's own HUD rendering does, and what the
		// user's brain expects from "flat panel floating in front of
		// me." A naive full-viewport composite (same pixels in both
		// eye buffers) doesn't fuse, since pixel (x, y) in the left
		// eye buffer represents a different physical direction than
		// pixel (x, y) in the right eye buffer.
		//
		// Geometry + convergence live in ComputeHUDQuad (HUDGeometry.h): one
		// eye-independent, head-centred panel both eyes share, so it fuses at
		// hudDepth. Iteration = registration order, so later HUD layers draw on
		// top. hudCoverage trims a comfort margin off the edges.
		const float hudDepth = std::max(0.3f, s.hudDepth);  // sanity floor
		const float coverage = std::clamp(s.hudCoverage, 0.5f, 1.0f);

		float projL[4], projR[4];
		Util::CachedProjectionRaw(vr::Eye_Left, projL[0], projL[1], projL[2], projL[3]);
		Util::CachedProjectionRaw(vr::Eye_Right, projR[0], projR[1], projR[2], projR[3]);
		const HUDQuad hudQuad = ComputeHUDQuad(projL, projR, hudDepth, coverage);

		const Matrix hudScale = Matrix::CreateScale(hudQuad.width, hudQuad.height, 1.0f);
		const Matrix hudOffset = Matrix::CreateTranslation(0.0f, hudQuad.centerY, -hudDepth);
		const Matrix hudModel = hudScale * hudOffset;
		const Matrix hudWvp = (hudModel * matrices.vpHeadSpace).Transpose();
		for (const auto& hud : hudClients) {
			if (!hud.texture)
				continue;
			auto* hudSRV = GetOrCreateHUDSRV(hud.client_id, hud.texture);
			if (!hudSRV)
				continue;
			ConstantBufferData cb;
			cb.wvp = hudWvp;
			DrawQuad(ctx, cb, hudSRV);
		}

		// Pass 2: panel-mode focused client (existing 3D quad logic).
		if (!wantPanelPass) {
			backup.Restore(ctx);
			return;
		}

		// HMD-attached pass.
		if (s.attachMode == Overlay::AttachMode::HMDOnly ||
			s.attachMode == Overlay::AttachMode::Both) {
			Matrix model;
			Matrix vpMat;
			if (s.positioningMethod == Overlay::PositioningMethod::FixedWorld) {
				model = Overlay::Config::CreateScaleMatrix(s.menuScale) * overlayState.fixedWorld.m;
				vpMat = matrices.vpWorldSpace;
			} else {
				Matrix offset = Matrix::CreateTranslation(s.hmdOffsetX, s.hmdOffsetY, s.hmdOffsetZ);
				model = Overlay::Config::CreateScaleMatrix(s.menuScale) * offset;
				vpMat = matrices.vpHeadSpace;
			}
			ConstantBufferData cb;
			cb.wvp = (model * vpMat).Transpose();
			DrawQuad(ctx, cb, panelSRV);
		}

		// Controller-attached pass (with backface culling).
		if (s.attachMode == Overlay::AttachMode::ControllerOnly ||
			s.attachMode == Overlay::AttachMode::Both) {
			const auto attachIdx = Util::GetControllerIndexForDevice(
				s.attachController, overlayState.lastKnownLeftHandedMode);
			if (attachIdx != vr::k_unTrackedDeviceIndexInvalid) {
				vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
				if (Util::GetDeviceToAbsoluteTrackingPoseCompatible(
						vr::TrackingUniverseStanding, 0, poses, vr::k_unMaxTrackedDeviceCount) &&
					poses[attachIdx].bPoseIsValid) {
					Matrix controllerWorld = Util::HmdMatrix34ToMatrix(
						poses[attachIdx].mDeviceToAbsoluteTracking);
					Matrix offset = Matrix::CreateTranslation(
						s.controllerOffsetX, s.controllerOffsetY, s.controllerOffsetZ);
					Matrix model = Overlay::Config::CreateScaleMatrix(s.menuScale) * offset * controllerWorld;

					// Backface culling: hide overlay from behind.
					Matrix overlayTransform = offset * controllerWorld;
					Vector3 overlayNormal(overlayTransform._31, overlayTransform._32, overlayTransform._33);
					overlayNormal.Normalize();
					Matrix hmdWorld = Util::HmdMatrix34ToMatrix(
						poses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking);
					Matrix eyeToHead = Util::HmdMatrix34ToMatrix(
						RE::BSOpenVR::GetSingleton()->vrSystem->GetEyeToHeadTransform(eye));
					Matrix eyeWorld = eyeToHead * hmdWorld;
					Vector3 toEye = eyeWorld.Translation() - overlayTransform.Translation();
					toEye.Normalize();
					if (overlayNormal.Dot(toEye) > 0.0f) {
						ConstantBufferData cb;
						cb.wvp = (model * matrices.vpWorldSpace).Transpose();
						DrawQuad(ctx, cb, panelSRV);
					}
				}
			}
		}

		backup.Restore(ctx);
	}
}
