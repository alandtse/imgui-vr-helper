// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// SKSE plugin entry point. Registers a messaging listener for the public
// API handshake (kMessage_GetInterface) and answers with a function pointer
// that returns the requested IImGuiVRHelperInterfaceNNN* singleton.

#include "pch.h"

#include "HelperImpl.h"
#include "Hooks.h"
#include "OpenVRDetection.h"
#include "Overlay.h"

#define IMGUI_VR_HELPER_STR_HELPER(x) #x
#define IMGUI_VR_HELPER_STR(x) IMGUI_VR_HELPER_STR_HELPER(x)
#define IMGUI_VR_HELPER_VERSION_STRING                 \
	IMGUI_VR_HELPER_STR(IMGUI_VR_HELPER_VERSION_MAJOR) \
	"." IMGUI_VR_HELPER_STR(IMGUI_VR_HELPER_VERSION_MINOR) "." IMGUI_VR_HELPER_STR(IMGUI_VR_HELPER_VERSION_PATCH)

namespace
{
	using namespace ImGuiVRHelperPluginAPI;

	/// Returns an interface pointer for the requested revision, or nullptr
	/// if the requested revision is newer than this build supports.
	void* GetApiFunction(uint32_t revision)
	{
		switch (revision) {
		case 1:
			return static_cast<IImGuiVRHelperInterface001*>(
				&ImGuiVRHelper::HelperImpl::GetSingleton());
		default:
			logs::warn("GetApiFunction: unsupported interface revision {}", revision);
			return nullptr;
		}
	}

	void OnSKSEMessage(SKSE::MessagingInterface::Message* msg)
	{
		if (!msg)
			return;

		// Diagnostic: log every message we receive so we can confirm what
		// SKSE is actually dispatching. Logs at info level so it stays
		// visible at default settings during the bring-up phase.
		logs::info("SKSE message: type={} sender='{}' dataLen={}",
			static_cast<uint32_t>(msg->type),
			msg->sender ? msg->sender : "<null>",
			msg->dataLen);

		// Handshake from a client requesting our API surface.
		if (msg->type == Message::kMessage_GetInterface &&
			msg->dataLen >= sizeof(Message*)) {
			auto* req = static_cast<Message*>(msg->data);
			req->GetApiFunction = &GetApiFunction;
			logs::info("Handshake from '{}' -> GetApiFunction installed",
				msg->sender ? msg->sender : "<unknown>");
			return;
		}

		switch (msg->type) {
		case SKSE::MessagingInterface::kPostLoad:
			// Load persisted settings as early as possible so InitD3D-time
			// resource allocation sees the user's configured offsets/scale.
			// Also write a default config to disk on first run so users
			// have a discoverable file to edit (e.g. to bump logLevel for
			// diagnostics without rebuilding).
			ImGuiVRHelper::Overlay::LoadSettings();
			ImGuiVRHelper::Overlay::WriteDefaultsIfMissing();
			ImGuiVRHelper::Overlay::ApplyLogLevel();
			break;

		case SKSE::MessagingInterface::kPostPostLoad:
			{
				// Install hooks here, NOT at kDataLoaded — by the time
				// kDataLoaded fires, Skyrim's renderer has already
				// initialized (BSGraphics::Renderer::InitD3D has been
				// called) and the thunk we'd write to that call site
				// would never execute. kPostPostLoad fires before the
				// renderer is up, so the thunk catches the original
				// call. This matches SCS's XSEPlugin.cpp:81-86 pattern.
				const auto info = VRDetection::Detect();
				logs::info("OpenVR runtime detection:");
				logs::info("  available={} compatible={} probing_ok={}",
					info.isAvailable, info.isCompatible, info.probingSucceeded);
				logs::info("  runtime={} version={} dll_size={}",
					VRDetection::RuntimeTypeToString(info.runtimeType),
					info.version, info.fileSize);
				if (!info.dllPath.empty()) {
					logs::info("  dll_path={}", info.dllPath);
				}
				logs::info("  interfaces: system={} overlay={} compositor={}",
					info.hasSystemInterface, info.hasOverlayInterface,
					info.hasCompositorInterface);

				ImGuiVRHelper::Hooks::Install();
			}
			break;

		case SKSE::MessagingInterface::kInputLoaded:
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			break;
		default:
			break;
		}
	}

}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);  // initializes logging via the bundled CommonLibSSE-NG default

	logs::info("ImGuiVRHelper {} loading", IMGUI_VR_HELPER_VERSION_STRING);

	if (!REL::Module::IsVR()) {
		logs::warn("Not running in Skyrim VR; helper will remain dormant.");
		// We still register the messaging listener so clients dispatching
		// the handshake don't time out. The interface methods will simply
		// return failure for everything until a VR runtime is detected.
	}

	auto* messaging = SKSE::GetMessagingInterface();
	if (!messaging) {
		SKSE::stl::report_and_fail("SKSE messaging interface unavailable"sv);
	}
	if (!messaging->RegisterListener(nullptr, OnSKSEMessage)) {
		SKSE::stl::report_and_fail("failed to register SKSE message listener"sv);
	}

	logs::info("ImGuiVRHelper loaded");
	return true;
}
