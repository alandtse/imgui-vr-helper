// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#pragma once

#include <array>
#include <memory>
#include <new>
#include <utility>

#include <Windows.h>
#include <d3d11_1.h>
#include <winrt/base.h>

namespace ImGuiVRHelper::Util
{
	/// Graphics-pipeline save/restore for contexts that cannot take SwapDeviceContextState.
	/// Saves wide slot ranges so a pass binding a neighbouring slot does not leak, and clears
	/// the GS/HS/DS stages the overlay never uses. Compute state, stream-out and resource writes
	/// are not covered.
	class ManualPipelineState final
	{
	public:
		void Save(ID3D11DeviceContext* c) noexcept
		{
			ID3D11RenderTargetView* rtvs[kRtvs] = {};
			ID3D11DepthStencilView* dsv = nullptr;
			c->OMGetRenderTargets(kRtvs, rtvs, &dsv);
			m_rtvCount = 0;
			for (UINT i = 0; i < kRtvs; ++i) {
				if (rtvs[i])
					m_rtvCount = i + 1;
			}
			Adopt(m_rtvs, rtvs);
			m_dsv.attach(dsv);
			ID3D11UnorderedAccessView* uavs[kUavs] = {};
			c->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, kUavs, uavs);
			Adopt(m_uavs, uavs);
			c->OMGetBlendState(m_blend.put(), m_blendFactor, &m_sampleMask);
			c->OMGetDepthStencilState(m_depthState.put(), &m_stencilRef);
			c->RSGetState(m_raster.put());
			m_numViewports = kViewports;
			c->RSGetViewports(&m_numViewports, m_viewports);

			m_vs.Save(c);
			m_ps.Save(c);
			m_gs.Save(c);
			m_hs.Save(c);
			m_ds.Save(c);

			ID3D11Buffer* vsCb[kCbs] = {};
			c->VSGetConstantBuffers(0, kCbs, vsCb);
			Adopt(m_vsCb, vsCb);
			ID3D11Buffer* psCb[kCbs] = {};
			c->PSGetConstantBuffers(0, kCbs, psCb);
			Adopt(m_psCb, psCb);
			ID3D11ShaderResourceView* srvs[kSrvs] = {};
			c->PSGetShaderResources(0, kSrvs, srvs);
			Adopt(m_psSrv, srvs);
			ID3D11SamplerState* samplers[kSamplers] = {};
			c->PSGetSamplers(0, kSamplers, samplers);
			Adopt(m_psSampler, samplers);

			ID3D11Buffer* vbs[kVbs] = {};
			c->IAGetVertexBuffers(0, kVbs, vbs, m_vbStride, m_vbOffset);
			Adopt(m_vb, vbs);
			c->IAGetIndexBuffer(m_ib.put(), &m_ibFormat, &m_ibOffset);
			c->IAGetInputLayout(m_layout.put());
			c->IAGetPrimitiveTopology(&m_topology);

			c->GSSetShader(nullptr, nullptr, 0);
			c->HSSetShader(nullptr, nullptr, 0);
			c->DSSetShader(nullptr, nullptr, 0);
		}

		void Restore(ID3D11DeviceContext* c) noexcept
		{
			// PS SRVs go first so an overlay SRV never aliases a restored RTV.
			ID3D11ShaderResourceView* nullSrvs[kSrvs] = {};
			c->PSSetShaderResources(0, kSrvs, nullSrvs);

			// OMSetRenderTargets drops pixel-pipeline UAVs, so rebind them with the targets.
			auto rtvs = Raw(m_rtvs);
			auto uavs = Raw(m_uavs);
			if (m_rtvCount < kUavs) {
				c->OMSetRenderTargetsAndUnorderedAccessViews(m_rtvCount, rtvs.data(), m_dsv.get(),
					m_rtvCount, kUavs - m_rtvCount, uavs.data() + m_rtvCount, nullptr);
			} else {
				c->OMSetRenderTargets(m_rtvCount, rtvs.data(), m_dsv.get());
			}
			c->OMSetBlendState(m_blend.get(), m_blendFactor, m_sampleMask);
			c->OMSetDepthStencilState(m_depthState.get(), m_stencilRef);
			c->RSSetState(m_raster.get());
			c->RSSetViewports(m_numViewports, m_viewports);

			m_vs.Restore(c);
			m_ps.Restore(c);
			m_gs.Restore(c);
			m_hs.Restore(c);
			m_ds.Restore(c);

			auto vsCb = Raw(m_vsCb);
			c->VSSetConstantBuffers(0, kCbs, vsCb.data());
			auto psCb = Raw(m_psCb);
			c->PSSetConstantBuffers(0, kCbs, psCb.data());
			auto srvs = Raw(m_psSrv);
			c->PSSetShaderResources(0, kSrvs, srvs.data());
			auto samplers = Raw(m_psSampler);
			c->PSSetSamplers(0, kSamplers, samplers.data());

			auto vbs = Raw(m_vb);
			c->IASetVertexBuffers(0, kVbs, vbs.data(), m_vbStride, m_vbOffset);
			c->IASetIndexBuffer(m_ib.get(), m_ibFormat, m_ibOffset);
			c->IASetInputLayout(m_layout.get());
			c->IASetPrimitiveTopology(m_topology);
		}

	private:
		static constexpr UINT kRtvs = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
		static constexpr UINT kUavs = D3D11_PS_CS_UAV_REGISTER_COUNT;
		static constexpr UINT kViewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		static constexpr UINT kCbs = 4;
		static constexpr UINT kSrvs = 8;
		static constexpr UINT kSamplers = 4;
		static constexpr UINT kVbs = 4;

		template <class T, std::size_t N>
		using Slots = std::array<winrt::com_ptr<T>, N>;

		template <class T, std::size_t N>
		static void Adopt(Slots<T, N>& dst, T* (&raw)[N]) noexcept
		{
			for (std::size_t i = 0; i < N; ++i)
				dst[i].attach(raw[i]);
		}

		template <class T, std::size_t N>
		static std::array<T*, N> Raw(const Slots<T, N>& slots) noexcept
		{
			std::array<T*, N> out{};
			for (std::size_t i = 0; i < N; ++i)
				out[i] = slots[i].get();
			return out;
		}

		Slots<ID3D11RenderTargetView, kRtvs> m_rtvs;
		UINT m_rtvCount = 0;
		Slots<ID3D11UnorderedAccessView, kUavs> m_uavs;
		winrt::com_ptr<ID3D11DepthStencilView> m_dsv;
		winrt::com_ptr<ID3D11BlendState> m_blend;
		FLOAT m_blendFactor[4] = {};
		UINT m_sampleMask = 0xFFFFFFFF;
		winrt::com_ptr<ID3D11DepthStencilState> m_depthState;
		UINT m_stencilRef = 0;
		winrt::com_ptr<ID3D11RasterizerState> m_raster;
		UINT m_numViewports = 0;
		D3D11_VIEWPORT m_viewports[kViewports] = {};

		// Class instances must be fetched: the getters write them through a null array when the
		// bound shader has any.
		template <class Shader,
			void (STDMETHODCALLTYPE ID3D11DeviceContext::*Get)(Shader**, ID3D11ClassInstance**, UINT*),
			void (STDMETHODCALLTYPE ID3D11DeviceContext::*Set)(Shader*, ID3D11ClassInstance* const*, UINT)>
		struct ShaderBinding
		{
			void Save(ID3D11DeviceContext* c) noexcept
			{
				ID3D11ClassInstance* raw[D3D11_SHADER_MAX_INTERFACES] = {};
				count = D3D11_SHADER_MAX_INTERFACES;
				(c->*Get)(shader.put(), raw, &count);
				for (UINT i = 0; i < count; ++i)
					instances[i].attach(raw[i]);
			}

			void Restore(ID3D11DeviceContext* c) noexcept
			{
				ID3D11ClassInstance* raw[D3D11_SHADER_MAX_INTERFACES] = {};
				for (UINT i = 0; i < count; ++i)
					raw[i] = instances[i].get();
				(c->*Set)(shader.get(), count ? raw : nullptr, count);
			}

			winrt::com_ptr<Shader> shader;
			std::array<winrt::com_ptr<ID3D11ClassInstance>, D3D11_SHADER_MAX_INTERFACES> instances;
			UINT count = 0;
		};

		ShaderBinding<ID3D11VertexShader, &ID3D11DeviceContext::VSGetShader, &ID3D11DeviceContext::VSSetShader> m_vs;
		ShaderBinding<ID3D11PixelShader, &ID3D11DeviceContext::PSGetShader, &ID3D11DeviceContext::PSSetShader> m_ps;
		ShaderBinding<ID3D11GeometryShader, &ID3D11DeviceContext::GSGetShader, &ID3D11DeviceContext::GSSetShader> m_gs;
		ShaderBinding<ID3D11HullShader, &ID3D11DeviceContext::HSGetShader, &ID3D11DeviceContext::HSSetShader> m_hs;
		ShaderBinding<ID3D11DomainShader, &ID3D11DeviceContext::DSGetShader, &ID3D11DeviceContext::DSSetShader> m_ds;

		Slots<ID3D11Buffer, kCbs> m_vsCb;
		Slots<ID3D11Buffer, kCbs> m_psCb;
		Slots<ID3D11ShaderResourceView, kSrvs> m_psSrv;
		Slots<ID3D11SamplerState, kSamplers> m_psSampler;

		Slots<ID3D11Buffer, kVbs> m_vb;
		UINT m_vbStride[kVbs] = {};
		UINT m_vbOffset[kVbs] = {};
		winrt::com_ptr<ID3D11Buffer> m_ib;
		DXGI_FORMAT m_ibFormat = DXGI_FORMAT_UNKNOWN;
		UINT m_ibOffset = 0;
		winrt::com_ptr<ID3D11InputLayout> m_layout;
		D3D11_PRIMITIVE_TOPOLOGY m_topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	};

	/// Render-thread pipeline isolation; check IsValid() before issuing commands.
	/// Nested guards require LIFO destruction; resource writes and active queries are not restored.
	/// Under RenderDoc the guard uses ManualPipelineState: its wrapped context faults in the next
	/// vrclient Submit after SwapDeviceContextState.
	class D3D11StateGuard final
	{
	public:
		[[nodiscard]] static bool RenderDocLoaded() noexcept
		{
			static const bool loaded = GetModuleHandleW(L"renderdoc.dll") != nullptr;
			return loaded;
		}

		explicit D3D11StateGuard(ID3D11DeviceContext* context, bool manual = RenderDocLoaded()) noexcept
		{
			if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
				return;

			if (manual) {
				m_manual.reset(new (std::nothrow) ManualPipelineState());
				if (!m_manual)
					return;
				m_manualContext.copy_from(context);
				m_manual->Save(context);
				return;
			}

			if (FAILED(context->QueryInterface(__uuidof(ID3D11DeviceContext1), m_context.put_void())))
				return;

			winrt::com_ptr<ID3D11Device> device;
			context->GetDevice(device.put());
			winrt::com_ptr<ID3D11Device1> device1;
			if (!device || FAILED(device->QueryInterface(__uuidof(ID3D11Device1), device1.put_void())) ||
				FAILED(device->QueryInterface(__uuidof(IUnknown), m_deviceIdentity.put_void())))
				return;

			auto& cache = CleanCache();
			if (cache.deviceIdentity.get() == m_deviceIdentity.get() && cache.state) {
				m_isolated = std::move(cache.state);
			} else {
				const D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
				const UINT flags = (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ?
				                       D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED :
				                       0;
				if (FAILED(device1->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION,
						__uuidof(ID3D11Device), nullptr, m_isolated.put())))
					return;
			}

			m_context->SwapDeviceContextState(m_isolated.get(), m_previous.put());
		}

		~D3D11StateGuard() noexcept
		{
			if (m_manual) {
				m_manual->Restore(m_manualContext.get());
				return;
			}
			if (!IsValid())
				return;

			m_context->ClearState();
			m_context->SwapDeviceContextState(m_previous.get(), nullptr);
			m_previous = nullptr;
			auto& cache = CleanCache();
			cache.state = std::move(m_isolated);
			cache.deviceIdentity = std::move(m_deviceIdentity);
		}

		D3D11StateGuard(const D3D11StateGuard&) = delete;
		D3D11StateGuard& operator=(const D3D11StateGuard&) = delete;
		D3D11StateGuard(D3D11StateGuard&&) = delete;
		D3D11StateGuard& operator=(D3D11StateGuard&&) = delete;

		[[nodiscard]] bool IsValid() const noexcept { return m_manual != nullptr || m_previous != nullptr; }

	private:
		struct Cache
		{
			winrt::com_ptr<IUnknown> deviceIdentity;
			winrt::com_ptr<ID3DDeviceContextState> state;
		};

		static Cache& CleanCache() noexcept
		{
			static thread_local Cache cache;
			return cache;
		}

		winrt::com_ptr<ID3D11DeviceContext1> m_context;
		winrt::com_ptr<IUnknown> m_deviceIdentity;
		winrt::com_ptr<ID3DDeviceContextState> m_isolated;
		winrt::com_ptr<ID3DDeviceContextState> m_previous;

		winrt::com_ptr<ID3D11DeviceContext> m_manualContext;
		std::unique_ptr<ManualPipelineState> m_manual;
	};
}
