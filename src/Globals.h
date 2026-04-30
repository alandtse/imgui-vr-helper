// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Helper-internal D3D11 references captured from Skyrim's renderer at
// BSGraphics::Renderer::InitD3D time. Subsequent ports (overlay textures,
// Present detour, InSceneOverlay fallback) read from here.
//
// Mirrors the pattern in SCS's globals::d3d but lives entirely inside the
// helper plugin's address space.

#pragma once

#include <atomic>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;

namespace ImGuiVRHelper::Globals
{
	/// Initialized once at BSGraphics::Renderer::InitD3D thunk-time. Until
	/// then all pointers are nullptr and `IsReady()` returns false.
	struct D3D
	{
		ID3D11Device* device = nullptr;
		ID3D11DeviceContext* context = nullptr;
		IDXGISwapChain* swapchain = nullptr;
	};

	/// Singleton accessor. The struct is initialized lazily; callers should
	/// check IsReady() before dereferencing.
	D3D& GetD3D();

	/// True iff the BSGraphics::Renderer::InitD3D hook has fired and the
	/// pointers are populated.
	bool IsReady();

	/// Set by the InitD3D hook thunk in Hooks.cpp.
	void SetD3D(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapchain);
}
