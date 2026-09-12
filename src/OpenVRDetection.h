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

	/// Ground truth for "is this really SteamVR": real SteamVR loads
	/// vrclient_x64.dll in-process; OpenComposite-family runtimes never do.
	/// Live-checked (not cached) -- vrclient_x64.dll loads only once the
	/// game calls VR_Init, which can be well after Detect() first runs, so
	/// callers that need an up-to-date answer (e.g. deciding whether
	/// IVROverlay calls are safe on the render thread) should call this
	/// directly instead of trusting a possibly-stale LastResult().
	bool IsVrclientLoaded();

	/// Probe loaded openvr_api.dll for the standard interface versions.
	bool ProbeRuntimeInterfaces(OpenVRDetectionResult& result);

	/// Read DLL path, version, file size, and modification time from the
	/// loaded openvr_api.dll module.
	void GatherDLLInfo(OpenVRDetectionResult& result);

	/// Heuristic classification of the openvr_api.dll: SteamVR vs
	/// OpenComposite, based on path, version, and file size.
	RuntimeType DetectRuntimeType(const std::string& dllPath, const std::string& version, uint64_t fileSize);

	/// Full detection: gather DLL info, classify runtime, probe interfaces.
	/// Caches its result for LastResult().
	OpenVRDetectionResult Detect();

	/// The result of the most recent Detect() call. Can go stale: the
	/// startup call (main.cpp, kPostPostLoad) can run before the game's
	/// VR_Init has loaded vrclient_x64.dll, misreporting real SteamVR as
	/// OpenComposite. Hooks.cpp re-probes after D3D init (by which point
	/// vrclient is loaded if it's ever going to be) and updates this.
	const OpenVRDetectionResult& LastResult();

	const char* RuntimeTypeToString(RuntimeType type);

	/// Log a full detection result at info level, in the standard multi-line
	/// format. Shared by the startup probe and any later re-probe.
	void LogDetectionResult(const OpenVRDetectionResult& info);
}
