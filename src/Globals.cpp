// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include "pch.h"

#include "Globals.h"

namespace ImGuiVRHelper::Globals
{
	namespace
	{
		D3D g_d3d;
		std::atomic_bool g_ready = false;
	}

	D3D& GetD3D()
	{
		return g_d3d;
	}

	bool IsReady()
	{
		return g_ready.load(std::memory_order_acquire);
	}

	void SetD3D(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapchain)
	{
		g_d3d.device = device;
		g_d3d.context = context;
		g_d3d.swapchain = swapchain;
		g_ready.store(true, std::memory_order_release);
		logs::info("D3D refs captured: device={}, context={}, swapchain={}",
			static_cast<void*>(device),
			static_cast<void*>(context),
			static_cast<void*>(swapchain));
	}
}
