// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// OpenVR runtime detection. Tells the helper whether SteamVR or
// OpenComposite is the active runtime (or whether OpenVR is unavailable
// entirely), and which interfaces are exposed by the loaded openvr_api.dll.
//
// Lifted from skyrim-community-shaders/src/Features/VR/OpenVRDetection.h
// with relicensing under GPL-3.0-or-later WITH the modding exception.

#pragma once

#include <cstdint>
#include <string>

namespace VRDetection
{
	enum class RuntimeType
	{
		Unknown,
		SteamVR,
		OpenComposite,
	};

	struct OpenVRDetectionResult
	{
		bool isAvailable = false;
		bool isCompatible = false;

		// Interface probing results
		bool hasOverlayInterface = false;
		bool hasSystemInterface = false;
		bool hasCompositorInterface = false;

		// File-based info
		std::string dllPath;
		std::string version;
		uint64_t fileSize = 0;
		std::string modificationTime;

		// Detection metadata
		RuntimeType runtimeType = RuntimeType::Unknown;
		bool probingSucceeded = false;
	};

	/// Live module-presence query only. OCU can itself use this filename,
	/// and both runtimes can be loaded together. This does NOT identify the
	/// active backend and must never select a submission thread.
	bool IsVrclientLoaded();

	/// Probe loaded openvr_api.dll for the standard interface versions.
	bool ProbeRuntimeInterfaces(OpenVRDetectionResult& result);

	/// Read DLL path, version, file size, and modification time from the
	/// loaded openvr_api.dll module.
	void GatherDLLInfo(OpenVRDetectionResult& result);

	/// Legacy diagnostic hint based on module names/path/version/size.
	/// Not evidence of active runtime ownership; never use for thread policy.
	RuntimeType DetectRuntimeType(const std::string& dllPath, const std::string& version, uint64_t fileSize);

	/// Full detection: gather DLL info, classify runtime, probe interfaces.
	/// Caches its result for LastResult().
	OpenVRDetectionResult Detect();

	/// The result of the most recent Detect() call. Startup metadata can go
	/// stale; Hooks.cpp re-probes after D3D init. Even then runtimeType is
	/// only a diagnostic hint, not a verified active-backend classification.
	const OpenVRDetectionResult& LastResult();

	const char* RuntimeTypeToString(RuntimeType type);

	/// Log a full detection result at info level, in the standard multi-line
	/// format. Shared by the startup probe and any later re-probe.
	void LogDetectionResult(const OpenVRDetectionResult& info);
}
