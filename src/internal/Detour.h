// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Manual COM vtable detour — equivalent to Detours::X64::DetourClassVTable,
// written inline so we don't vendor MS Detours just for the two hooks
// (IDXGISwapChain::Present, IVRCompositor::Submit) that need it.

#pragma once

#include <Windows.h>

#include <cstddef>

namespace ImGuiVRHelper::Util
{
	/// Replace vtable `slot` of COM object `obj` with `replacement`, returning
	/// the previous pointer (call it to chain to the original).
	template <class FnPtr>
	FnPtr DetourClassVTable(void* obj, std::size_t slot, FnPtr replacement)
	{
		auto** vtable = *reinterpret_cast<void***>(obj);
		DWORD oldProtect = 0;
		VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &oldProtect);
		FnPtr original = reinterpret_cast<FnPtr>(vtable[slot]);
		vtable[slot] = reinterpret_cast<void*>(replacement);
		VirtualProtect(&vtable[slot], sizeof(void*), oldProtect, &oldProtect);
		return original;
	}
}
