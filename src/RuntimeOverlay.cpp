// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include "pch.h"

#include "RuntimeOverlay.h"

#include "Globals.h"
#include "HelperImpl.h"
#include "InSceneOverlay.h"
#include "Overlay.h"
#include "OverlayTinter.h"
#include "internal/RuntimeOverlayCore.h"
#include "internal/RuntimeOverlayProvenance.h"
#include "internal/VRUtils.h"

#include <RE/B/BSOpenVR.h>
#include <openvr.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace ImGuiVRHelper::RuntimeOverlay
{
	using Core::Backend;
	using Core::State;

	namespace
	{
		constexpr vr::EVREye kOwnerEye = vr::Eye_Left;
		// A stopped owner cannot remove what the runtime still shows.
		constexpr std::uint64_t kMaxUnfedPresentTicks = 8;

		constexpr UINT kLayerBindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		constexpr UINT kLayerMiscFlags = D3D11_RESOURCE_MISC_SHARED;

		struct LayerTexture
		{
			winrt::com_ptr<ID3D11Texture2D> texture;
			Core::ImageDesc desc{};
		};

		struct Sink final : Core::ISink
		{
			void Log(Core::LogLevel level, const std::string& message) override
			{
				switch (level) {
				case Core::LogLevel::Info:
					logs::info("{}", message);
					break;
				case Core::LogLevel::Warn:
					logs::warn("{}", message);
					break;
				case Core::LogLevel::Error:
					logs::error("{}", message);
					break;
				}
			}
		};

		struct OverlayApi final : Core::IOverlayApi
		{
			static constexpr int kUnavailable = vr::VROverlayError_UnknownOverlay;

			void Refresh()
			{
				const Util::OpenVRContext ctx{};
				openvr_ = ctx.openvr;
				system_ = ctx.system;
				overlay_ = ctx.overlay;
				textureOverlay_ = RE::BSOpenVR::GetCleanIVROverlay();
				authorized_ = true;
			}

			void Revoke() noexcept
			{
				authorized_ = false;
				openvr_ = nullptr;
				system_ = nullptr;
				overlay_ = nullptr;
				textureOverlay_ = nullptr;
			}

			bool Available() const override
			{
				return authorized_ && openvr_ && system_ && overlay_;
			}

			int CreateOverlay(const char* key, const char* name, std::uint64_t& out) override
			{
				if (!overlay_)
					return kUnavailable;
				vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
				const int error = static_cast<int>(overlay_->CreateOverlay(key, name, &handle));
				if (error == 0)
					out = handle;
				return error;
			}

			int DestroyOverlay(std::uint64_t handle) override
			{
				return overlay_ ? static_cast<int>(overlay_->DestroyOverlay(handle)) : kUnavailable;
			}

			int ShowOverlay(std::uint64_t handle) override
			{
				return overlay_ ? static_cast<int>(overlay_->ShowOverlay(handle)) : kUnavailable;
			}

			int HideOverlay(std::uint64_t handle) override
			{
				return overlay_ ? static_cast<int>(overlay_->HideOverlay(handle)) : kUnavailable;
			}

			int ClearOverlayTexture(std::uint64_t handle) override
			{
				return overlay_ ? static_cast<int>(overlay_->ClearOverlayTexture(handle)) : kUnavailable;
			}

			int SetOverlayTexture(std::uint64_t handle, const void* texture) override
			{
				// SteamVR denies texture submission through the game's proxied
				// interface; only the clean implementation may receive it.
				if (!textureOverlay_)
					return kUnavailable;
				vr::Texture_t submitted{ const_cast<void*>(texture), vr::TextureType_DirectX,
					vr::ColorSpace_Auto };
				return static_cast<int>(textureOverlay_->SetOverlayTexture(handle, &submitted));
			}

			int SetOverlayWidthInMeters(std::uint64_t handle, float width) override
			{
				return overlay_ ? static_cast<int>(overlay_->SetOverlayWidthInMeters(handle, width)) : kUnavailable;
			}

			int SetOverlayTransformAbsolute(std::uint64_t handle, int origin,
				const float transform[3][4]) override
			{
				if (!overlay_)
					return kUnavailable;
				vr::HmdMatrix34_t matrix{};
				std::memcpy(matrix.m, transform, sizeof(matrix.m));
				return static_cast<int>(overlay_->SetOverlayTransformAbsolute(handle,
					static_cast<vr::ETrackingUniverseOrigin>(origin), &matrix));
			}

			const char* ErrorName(int error) const override
			{
				return overlay_ ? overlay_->GetOverlayErrorNameFromEnum(
									  static_cast<vr::VROverlayError>(error)) :
				                  "overlay error";
			}

			RE::BSOpenVR* openvr_ = nullptr;
			vr::IVRSystem* system_ = nullptr;
			vr::IVROverlay* overlay_ = nullptr;
			vr::IVROverlay* textureOverlay_ = nullptr;
			bool authorized_ = false;
		};

		struct LayerSource final : Core::IFrameSource
		{
			bool Prepare(std::uint32_t focusedClient, Core::PreparedImage& out,
				const char*& reason) override
			{
				out = Core::PreparedImage{};
				auto* device = Globals::GetD3D().device;
				auto* ctx = Globals::GetD3D().context;
				if (!device || !ctx) {
					reason = "the game D3D device is unavailable";
					return false;
				}
				if (focusedClient == 0) {
					reason = "no focused client";
					return false;
				}
				auto panel = HelperImpl::GetSingleton().GetClientPanelTexture(focusedClient);
				if (!panel) {
					reason = "the focused client has no panel texture";
					return false;
				}
				auto tinted = OverlayTinter::GetOutputTexture(panel.get());
				ID3D11Texture2D* image = tinted ? tinted.get() : panel.get();

				D3D11_TEXTURE2D_DESC source{};
				image->GetDesc(&source);
				winrt::com_ptr<ID3D11Device> imageDevice;
				image->GetDevice(imageDevice.put());
				if (imageDevice.get() != device) {
					out.transient = false;
					reason = "the panel texture is not on the game D3D device";
					return false;
				}

				Core::ImageDesc desc{};
				desc.width = source.Width;
				desc.height = source.Height;
				desc.mipLevels = source.MipLevels;
				desc.arraySize = source.ArraySize;
				desc.sampleCount = source.SampleDesc.Count;
				desc.format = static_cast<std::uint32_t>(source.Format);
				desc.device = device;
				if (!Core::IsCopyCompatible(desc)) {
					out.transient = false;
					reason = "the panel texture layout cannot be copied as a menu layer";
					return false;
				}

				if (!layer.texture || Core::NeedsRecreate(layer.desc, desc)) {
					if (layer.desc.device != nullptr && layer.desc.device != device)
						out.deviceChanged = true;
					D3D11_TEXTURE2D_DESC layerDesc{};
					layerDesc.Width = desc.width;
					layerDesc.Height = desc.height;
					layerDesc.MipLevels = 1;
					layerDesc.ArraySize = 1;
					layerDesc.Format = source.Format;
					layerDesc.SampleDesc.Count = 1;
					layerDesc.Usage = D3D11_USAGE_DEFAULT;
					layerDesc.BindFlags = kLayerBindFlags;
					layerDesc.MiscFlags = kLayerMiscFlags;
					winrt::com_ptr<ID3D11Texture2D> fresh;
					const HRESULT hr = device->CreateTexture2D(&layerDesc, nullptr, fresh.put());
					if (FAILED(hr)) {
						logs::error("RuntimeOverlay: CreateTexture2D failed (HRESULT 0x{:08X})",
							static_cast<std::uint32_t>(hr));
						out = Core::PreparedImage{};
						out.transient = false;
						reason = "could not allocate the menu layer texture";
						return false;
					}
					layer.texture = std::move(fresh);
					layer.desc = desc;
					++generation;
				}

				ctx->CopyResource(layer.texture.get(), image);
				auto& helper = HelperImpl::GetSingleton();
				if ((helper.GetClientFlags(focusedClient) &
						ImGuiVRHelperPluginAPI::kClientFlag_OwnCursor) == 0) {
					InSceneOverlay::RenderCursorIntoPanel(layer.texture.get());
				}

				out.ok = true;
				out.texture = layer.texture.get();
				out.generation = generation;
				out.desc = desc;
				return true;
			}

			LayerTexture layer;
			std::uint64_t generation = 0;
		};

		Sink g_sink;
		OverlayApi g_api;
		LayerSource g_source;
		Core::ThreadGate g_gate{ g_sink };

		Core::Host& Host()
		{
			static Core::Host host{ g_api, g_source, g_sink };
			return host;
		}

		std::atomic<std::uint64_t> g_lastOwnerPresentTick{ 0 };
		std::atomic<std::uint64_t> g_presentTicks{ 0 };
		std::atomic<bool> g_stalled{ false };
		std::atomic<bool> g_stallLogged{ false };

		Core::DetectionBudget g_detection;

		struct Placement
		{
			Overlay::AttachMode attachMode = Overlay::AttachMode::None;
			Overlay::PositioningMethod positioningMethod = Overlay::PositioningMethod::FixedWorld;
			ImGuiVRHelperPluginAPI::InputDeviceType attachController =
				ImGuiVRHelperPluginAPI::InputDeviceType::Primary;

			bool operator==(const Placement& other) const
			{
				return attachMode == other.attachMode &&
				       positioningMethod == other.positioningMethod &&
				       attachController == other.attachController;
			}
		};
		Placement g_placement;
		bool g_placementValid = false;
		std::uint64_t g_configGeneration = 0;

		std::wstring ModulePathFor(const void* address)
		{
			HMODULE module = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
										GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(address), &module) ||
				!module) {
				return {};
			}
			wchar_t path[MAX_PATH]{};
			const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
			return length ? std::wstring(path, length) : std::wstring{};
		}

		std::wstring GameDirectory()
		{
			wchar_t path[MAX_PATH]{};
			const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
			if (!length)
				return {};
			std::wstring file(path, length);
			const auto slash = file.find_last_of(L"\\/");
			return slash == std::wstring::npos ? std::wstring{} : file.substr(0, slash + 1);
		}

		std::wstring WidenUtf8(std::string_view text)
		{
			if (text.empty())
				return {};
			const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(),
				static_cast<int>(text.size()), nullptr, 0);
			if (length <= 0)
				return {};
			std::wstring out(static_cast<std::size_t>(length), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
				length);
			return out;
		}

		std::vector<std::wstring> InstalledRuntimeRoots()
		{
			static const std::vector<std::wstring> roots = [] {
				wchar_t localAppData[MAX_PATH]{};
				const DWORD length =
					GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
				if (length == 0 || length >= MAX_PATH)
					return std::vector<std::wstring>{};
				std::wstring file(localAppData, length);
				file += L"\\openvr\\openvrpaths.vrpath";
				std::ifstream stream(file, std::ios::binary);
				if (!stream)
					return std::vector<std::wstring>{};
				const std::string text((std::istreambuf_iterator<char>(stream)),
					std::istreambuf_iterator<char>());
				std::vector<std::wstring> out;
				for (const auto& root : Provenance::ParseRuntimeRoots(text))
					out.push_back(WidenUtf8(root));
				return out;
			}();
			return roots;
		}

		Backend ClassifyCleanOverlay()
		{
			auto* clean = RE::BSOpenVR::GetCleanIVROverlay();
			if (!clean)
				return Backend::Proxy;
			void** vtable = *reinterpret_cast<void***>(clean);
			if (!vtable || !vtable[0])
				return Backend::Unknown;
			const auto module = ModulePathFor(vtable[0]);
			const auto identity = Provenance::ReadModuleIdentity(module);
			switch (Provenance::ClassifyRuntimeModule(module, InstalledRuntimeRoots(),
				GameDirectory(), identity)) {
			case Provenance::Runtime::SteamVR:
				return Backend::SteamVR;
			case Provenance::Runtime::OpenComposite:
				return Backend::OpenComposite;
			case Provenance::Runtime::Proxy:
				return Backend::Proxy;
			default:
				return Backend::Unknown;
			}
		}

		Backend DetectBackend()
		{
			Util::OpenVRContext ctx;
			if (!ctx.IsValid())
				return Backend::Unknown;

			char tracking[128]{};
			vr::ETrackedPropertyError perr = vr::TrackedProp_Success;
			ctx.system->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
				vr::Prop_TrackingSystemName_String, tracking, sizeof(tracking), &perr);
			if (perr == vr::TrackedProp_Success && _stricmp(tracking, "null") == 0) {
				logs::info("RuntimeOverlay: headless VR driver; native hosting stays off");
				return Backend::Headless;
			}
			if (!ctx.HasOverlay()) {
				logs::info("RuntimeOverlay: the runtime exposes no IVROverlay; staying in-scene");
				return Backend::Unsupported;
			}

			vr::TrackedDevicePose_t head{};
			if (!Util::GetDeviceToAbsoluteTrackingPoseCompatible(vr::TrackingUniverseStanding, 0,
					&head, 1) ||
				!head.bPoseIsValid) {
				return Backend::Unknown;
			}

			const auto backend = ClassifyCleanOverlay();
			switch (backend) {
			case Backend::SteamVR:
				logs::info(
					"RuntimeOverlay: SteamVR runtime implementation identified; hosting "
					"the menu layer");
				break;
			case Backend::OpenComposite:
				logs::info(
					"RuntimeOverlay: OpenComposite has no verified overlay presentation "
					"capability; staying in-scene");
				break;
			case Backend::Proxy:
				logs::info(
					"RuntimeOverlay: the overlay interface resolves to a proxy, not the "
					"installed runtime implementation; staying in-scene");
				break;
			default:
				logs::info("RuntimeOverlay: unrecognized overlay implementation; staying in-scene");
				break;
			}
			return backend;
		}

		Backend ResolveBackend()
		{
			switch (g_detection.Next()) {
			case Core::ProbeDecision::Cached:
				return g_detection.Cached();
			case Core::ProbeDecision::Exhausted:
				logs::info(
					"RuntimeOverlay: the runtime never reported a usable head pose; "
					"staying in-scene");
				return g_detection.Cached();
			case Core::ProbeDecision::Probe:
				break;
			}
			const auto backend = DetectBackend();
			g_detection.Record(backend);
			return backend;
		}

		vr::ETrackingUniverseOrigin CurrentTrackingOrigin()
		{
			auto* compositor = RE::BSOpenVR::GetIVRCompositor();
			return compositor ? compositor->GetTrackingSpace() : vr::TrackingUniverseStanding;
		}

		void CopyTransform(const DirectX::SimpleMath::Matrix& matrix, float out[3][4])
		{
			const auto hmd = Util::MatrixToHmdMatrix34(matrix);
			std::memcpy(out, hmd.m, sizeof(hmd.m));
		}

		void ResolveAnchors(const Overlay::Settings& s, const Overlay::State& state,
			Core::TickInput& in)
		{
			vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
			const bool havePoses = Util::GetDeviceToAbsoluteTrackingPoseCompatible(
				static_cast<vr::ETrackingUniverseOrigin>(in.origin), 0, poses,
				vr::k_unMaxTrackedDeviceCount);

			if ((in.requestedMask & (1u << Core::kHmdAnchor)) != 0) {
				DirectX::SimpleMath::Matrix anchor;
				bool headSpace = false;
				if (InSceneOverlay::ResolveAnchorWorld(Overlay::OverlayType::HMD, s, state, anchor,
						headSpace)) {
					bool placed = true;
					if (headSpace) {
						if (!havePoses || !poses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid) {
							placed = false;
						} else {
							anchor = anchor * Util::HmdMatrix34ToMatrix(
												  poses[vr::k_unTrackedDeviceIndex_Hmd]
													  .mDeviceToAbsoluteTracking);
						}
					}
					if (placed) {
						in.anchor[Core::kHmdAnchor].placed = true;
						CopyTransform(anchor, in.anchor[Core::kHmdAnchor].transform);
					}
				}
			}

			if ((in.requestedMask & (1u << Core::kControllerAnchor)) != 0) {
				DirectX::SimpleMath::Matrix anchor;
				bool headSpace = false;
				if (InSceneOverlay::ResolveAnchorWorld(Overlay::OverlayType::Controller, s, state,
						anchor, headSpace)) {
					in.anchor[Core::kControllerAnchor].placed = true;
					CopyTransform(anchor, in.anchor[Core::kControllerAnchor].transform);
				}
			}
		}

		std::uint32_t AnchorMask(Overlay::AttachMode mode)
		{
			switch (mode) {
			case Overlay::AttachMode::HMDOnly:
				return 1u << Core::kHmdAnchor;
			case Overlay::AttachMode::ControllerOnly:
				return 1u << Core::kControllerAnchor;
			case Overlay::AttachMode::Both:
				return (1u << Core::kHmdAnchor) | (1u << Core::kControllerAnchor);
			case Overlay::AttachMode::None:
				break;
			}
			return 0;
		}

		void UpdateConfigGeneration(const Overlay::Settings& s)
		{
			Placement next;
			next.attachMode = s.attachMode;
			next.positioningMethod = s.positioningMethod;
			next.attachController = s.attachController;
			if (!g_placementValid || !(next == g_placement))
				++g_configGeneration;
			g_placement = next;
			g_placementValid = true;
		}

		void RunOwnedTick(Core::Host& host, bool requested)
		{
			auto& overlayState = Overlay::State::GetSingleton();
			const auto& s = overlayState.settings;

			Core::TickInput in;
			in.requested = requested;
			in.renderPathDisabled = InSceneOverlay::IsRenderPathDisabled();
			in.presentProducerEstablished = true;
			in.presentProducerMatches = true;

			if (in.requested) {
				in.backend = ResolveBackend();
				const std::uint32_t focused = HelperImpl::GetSingleton().GetFocusedClientId();
				in.focusedClient = focused;
				UpdateConfigGeneration(s);
				in.configGeneration = g_configGeneration;
				if (focused != 0 && s.attachMode != Overlay::AttachMode::None) {
					in.requestedMask = AnchorMask(s.attachMode);
					in.widthMeters = std::clamp(s.menuScale, Overlay::Config::kMinMenuScale,
						Overlay::Config::kMaxMenuScale);
					in.origin = static_cast<int>(CurrentTrackingOrigin());
					ResolveAnchors(s, overlayState, in);
				}
			}

			if (in.requested || host.NeedsRuntime())
				g_api.Refresh();
			host.OwnerTick(in);
		}
	}

	void RenderTick()
	{
		const auto thread = static_cast<std::uint64_t>(GetCurrentThreadId());
		g_gate.EnterPresent(thread);

		const auto present = g_presentTicks.fetch_add(1, std::memory_order_relaxed) + 1;
		if (!Host().Hosting()) {
			g_stalled.store(false, std::memory_order_relaxed);
			g_stallLogged.store(false, std::memory_order_relaxed);
			return;
		}
		const auto lastOwner = g_lastOwnerPresentTick.load(std::memory_order_relaxed);
		if (lastOwner == 0 || present - lastOwner < kMaxUnfedPresentTicks)
			return;
		if (!g_stalled.exchange(true, std::memory_order_relaxed) &&
			!g_stallLogged.exchange(true, std::memory_order_relaxed)) {
			logs::error(
				"RuntimeOverlay: the submit tick stopped updating the menu; the "
				"in-scene menu resumes, and the native layer may stay visible until the "
				"runtime accepts overlay calls again");
		}
	}

	void SubmitTick(vr::EVREye eye)
	{
		if (eye != kOwnerEye)
			return;

		const auto thread = static_cast<std::uint64_t>(GetCurrentThreadId());
		Core::OwnershipObservation ownership;
		if (g_gate.EnterOwnerTick(thread, ownership) == Core::TickAccess::Refused) {
			return;
		}

		g_lastOwnerPresentTick.store(g_presentTicks.load(std::memory_order_relaxed),
			std::memory_order_relaxed);
		if (g_stalled.exchange(false, std::memory_order_relaxed))
			g_stallLogged.store(false, std::memory_order_relaxed);

		auto& host = Host();
		bool requested = false;
		Core::OwnerTickWork work;
		work.standDown = [&](const char* status) {
			g_api.Revoke();
			host.OwnerBlocked(status);
		};
		work.requested = [&] {
			requested = Overlay::State::GetSingleton().settings.useRuntimeOverlay;
			g_detection.ObserveSetting(requested, host.CleanupPending());
			return requested;
		};
		work.needsOwnerWork = [&] { return host.NeedsOwnerWork(); };
		work.run = [&] { RunOwnedTick(host, requested); };
		Core::RunOwnerTick(ownership, work);
	}

	void ReportFault(const char* reason)
	{
		Host().RequestFault(reason);
	}

	bool IsHostingPanel()
	{
		return Core::HostingObservable(Host().Hosting(),
			g_stalled.load(std::memory_order_relaxed), g_gate.PresentMigrated(),
			g_gate.ForeignThreadSeen());
	}

	const char* StatusText()
	{
		auto& host = Host();
		return Core::ObservedStatus(host.StatusText(),
			g_stalled.load(std::memory_order_relaxed), g_gate.PresentMigrated(),
			g_gate.ForeignThreadSeen(), host.CleanupPending(), host.HoldsHandles());
	}
}
