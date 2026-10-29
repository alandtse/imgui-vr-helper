// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#pragma once

#include <utility>

#include <d3d11_1.h>
#include <winrt/base.h>

namespace ImGuiVRHelper::Util
{
	/// Render-thread pipeline isolation; check IsValid() before issuing commands.
	/// Nested guards require LIFO destruction; resource writes and active queries are not restored.
	class D3D11StateGuard final
	{
	public:
		explicit D3D11StateGuard(ID3D11DeviceContext* context) noexcept
		{
			if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
				return;
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

		[[nodiscard]] bool IsValid() const noexcept { return m_previous != nullptr; }

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
	};
}
