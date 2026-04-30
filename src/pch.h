// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <RE/Skyrim.h>
#include <REX/REX.h>
#include <SKSE/SKSE.h>

#include <Windows.h>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <d3d11.h>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <winrt/base.h>

namespace logs = SKSE::log;
using namespace std::literals;

// Helper-side equivalent of SCS's stl::write_thunk_call<T>(addr) — wraps
// SKSE::GetTrampoline().write_call<N>() so a thunk struct with `static
// void thunk(...)` and `static REL::Relocation<...> func` can be installed
// in one call.
namespace stl
{
	using namespace SKSE::stl;

	template <class T, std::size_t Size = 5>
	void write_thunk_call(std::uintptr_t a_src)
	{
		auto& trampoline = SKSE::GetTrampoline();
		if constexpr (Size == 6) {
			T::func = *reinterpret_cast<std::uintptr_t*>(trampoline.write_call<6>(a_src, T::thunk));
		} else {
			T::func = trampoline.write_call<Size>(a_src, T::thunk);
		}
	}
}
