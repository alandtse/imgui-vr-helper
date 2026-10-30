// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#pragma once

#ifndef NOMINMAX
#	define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winver.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cwchar>
#include <string>
#include <string_view>
#include <vector>

namespace ImGuiVRHelper::RuntimeOverlay::Provenance
{
	enum class Runtime : std::uint8_t
	{
		Unknown,
		SteamVR,
		OpenComposite,
		Proxy,
	};

	inline constexpr wchar_t kOpenVrApiModule[] = L"openvr_api.dll";

	struct ModuleIdentity
	{
		bool available = false;
		std::wstring company;
		std::wstring product;
	};

	inline std::wstring NormalizeModulePath(std::wstring_view path)
	{
		std::wstring out(path);
		for (auto& c : out) {
			if (c == L'/')
				c = L'\\';
			if (c >= L'A' && c <= L'Z')
				c = static_cast<wchar_t>(c - L'A' + L'a');
		}
		while (!out.empty() && out.back() == L'\\')
			out.pop_back();
		return out;
	}

	inline bool PathIsUnder(const std::wstring& path, const std::wstring& root)
	{
		if (root.empty())
			return false;
		if (path == root)
			return true;
		return path.size() > root.size() && path.compare(0, root.size(), root) == 0 &&
		       path[root.size()] == L'\\';
	}

	inline std::wstring FileName(const std::wstring& path)
	{
		const auto slash = path.find_last_of(L'\\');
		return slash == std::wstring::npos ? path : path.substr(slash + 1);
	}

	inline bool EqualsNoCase(std::wstring_view a, std::wstring_view b)
	{
		if (a.size() != b.size())
			return false;
		for (std::size_t i = 0; i < a.size(); ++i) {
			wchar_t x = a[i];
			wchar_t y = b[i];
			if (x >= L'A' && x <= L'Z')
				x = static_cast<wchar_t>(x - L'A' + L'a');
			if (y >= L'A' && y <= L'Z')
				y = static_cast<wchar_t>(y - L'A' + L'a');
			if (x != y)
				return false;
		}
		return true;
	}

	/// The stock SteamVR runtime reports this Win32 version identity.
	inline bool IsSteamVrImplementation(const ModuleIdentity& identity)
	{
		return identity.available && EqualsNoCase(identity.company, L"Valve Corporation") &&
		       EqualsNoCase(identity.product, L"VR Client");
	}

	inline std::wstring ReadVersionString(void* block, WORD language, WORD codePage,
		const wchar_t* key)
	{
		wchar_t prefix[64]{};
		if (std::swprintf(prefix, std::size(prefix), L"\\StringFileInfo\\%04x%04x\\", language,
				codePage) < 0) {
			return {};
		}
		const std::wstring query = std::wstring(prefix) + key;
		wchar_t* value = nullptr;
		UINT length = 0;
		if (!VerQueryValueW(block, query.c_str(), reinterpret_cast<void**>(&value), &length) ||
			!value) {
			return {};
		}
		return std::wstring(value, length ? length - 1 : 0);
	}

	/// Reads Win32 version metadata for a module path. A missing file or block
	/// yields an unavailable identity, which callers treat as a fallback.
	inline ModuleIdentity ReadModuleIdentity(const std::wstring& path)
	{
		ModuleIdentity identity;
		if (path.empty())
			return identity;
		DWORD ignored = 0;
		const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
		if (size == 0)
			return identity;
		std::vector<char> data(size);
		if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
			return identity;

		struct Translation
		{
			WORD language;
			WORD codePage;
		};

		Translation* translations = nullptr;
		UINT bytes = 0;
		std::wstring company;
		std::wstring product;
		if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
				reinterpret_cast<void**>(&translations), &bytes) &&
			translations && bytes >= sizeof(Translation)) {
			const UINT entries = bytes / sizeof(Translation);
			for (UINT i = 0; i < entries && company.empty(); ++i) {
				const auto language = translations[i].language;
				const auto codePage = translations[i].codePage;
				company = ReadVersionString(data.data(), language, codePage, L"CompanyName");
				product = ReadVersionString(data.data(), language, codePage, L"ProductName");
			}
		}
		if (company.empty()) {
			company = ReadVersionString(data.data(), 0x0409, 0x04b0, L"CompanyName");
			product = ReadVersionString(data.data(), 0x0409, 0x04b0, L"ProductName");
		}
		identity.company = std::move(company);
		identity.product = std::move(product);
		identity.available = !identity.company.empty() || !identity.product.empty();
		return identity;
	}

	/// Classifies the module that implements the clean IVROverlay vtable. SteamVR
	/// requires the stock implementation location under a registered runtime root
	/// and Valve's version identity; a location or file name alone never promotes
	/// a wrapper.
	inline Runtime ClassifyRuntimeModule(std::wstring modulePath,
		const std::vector<std::wstring>& runtimeRoots, std::wstring gameDirectory,
		const ModuleIdentity& identity = {})
	{
		if (modulePath.empty())
			return Runtime::Unknown;
		modulePath = NormalizeModulePath(modulePath);
		gameDirectory = NormalizeModulePath(gameDirectory);
		const auto fileName = FileName(modulePath);
		const bool inGame = !gameDirectory.empty() && PathIsUnder(modulePath, gameDirectory);

		if (inGame) {
			return fileName == kOpenVrApiModule ||
			               modulePath.find(L"opencomposite") != std::wstring::npos ?
			           Runtime::OpenComposite :
			           Runtime::Proxy;
		}

		for (const auto& rawRoot : runtimeRoots) {
			const auto root = NormalizeModulePath(rawRoot);
			if (!PathIsUnder(modulePath, root))
				continue;
			const auto stockModule = NormalizeModulePath(root + L"\\bin\\vrclient_x64.dll");
			if (modulePath != stockModule)
				return Runtime::Proxy;
			if (!identity.available)
				return Runtime::Unknown;
			return IsSteamVrImplementation(identity) ? Runtime::SteamVR : Runtime::Proxy;
		}

		return fileName == kOpenVrApiModule ||
		               modulePath.find(L"opencomposite") != std::wstring::npos ?
		           Runtime::OpenComposite :
		           Runtime::Unknown;
	}

	/// Extracts the runtime roots from an openvrpaths.vrpath document. A malformed
	/// or absent file yields no roots, which keeps classification conservative.
	inline std::vector<std::string> ParseRuntimeRoots(std::string_view json)
	{
		std::vector<std::string> roots;
		try {
			const auto doc = nlohmann::json::parse(json.begin(), json.end());
			const auto it = doc.find("runtime");
			if (it == doc.end() || !it->is_array())
				return roots;
			for (const auto& entry : *it) {
				if (entry.is_string())
					roots.push_back(entry.get<std::string>());
			}
		} catch (const nlohmann::json::exception&) {
		}
		return roots;
	}
}
