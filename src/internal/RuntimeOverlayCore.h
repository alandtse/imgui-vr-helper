// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace ImGuiVRHelper::RuntimeOverlay::Core
{
	enum class Backend : std::uint8_t
	{
		Unknown,
		SteamVR,
		OpenComposite,
		Proxy,
		Headless,
		Unsupported,
	};

	enum class State : std::uint8_t
	{
		Disabled,
		Requesting,
		Active,
		Standing,
		CleaningUp,
	};

	enum class LogLevel : std::uint8_t
	{
		Info,
		Warn,
		Error,
	};

	constexpr std::uint32_t kAnchorCount = 2;
	constexpr std::uint32_t kHmdAnchor = 0;
	constexpr std::uint32_t kControllerAnchor = 1;
	constexpr std::uint64_t kInvalidHandle = 0;

	constexpr int kConfirmFrames = 2;
	constexpr int kMaxApplyFailures = 3;
	constexpr int kDetectRetryTicks = 60;
	constexpr int kMaxDetectAttempts = 10;

	enum class ProbeDecision : std::uint8_t
	{
		Cached,
		Probe,
		Exhausted,
	};

	/// Bounded backend probing. The cached result survives until the advertised
	/// setting is observed off and on again after pending cleanup finishes.
	class DetectionBudget
	{
	public:
		Backend Cached() const { return cached_; }

		ProbeDecision Next()
		{
			if (cached_ != Backend::Unknown)
				return ProbeDecision::Cached;
			if (countdown_ > 0) {
				--countdown_;
				return ProbeDecision::Cached;
			}
			if (++attempts_ > kMaxDetectAttempts) {
				cached_ = Backend::Unsupported;
				return ProbeDecision::Exhausted;
			}
			return ProbeDecision::Probe;
		}

		void Record(Backend result)
		{
			cached_ = result;
			countdown_ = kDetectRetryTicks;
		}

		/// Returns true when a fresh probe budget was granted. Owner-only.
		bool ObserveSetting(bool requested, bool cleanupPending)
		{
			if (!requested) {
				offObserved_ = true;
				return false;
			}
			if (!offObserved_ || cleanupPending)
				return false;
			offObserved_ = false;
			cached_ = Backend::Unknown;
			attempts_ = 0;
			countdown_ = 0;
			return true;
		}

		int Attempts() const { return attempts_; }

	private:
		Backend cached_ = Backend::Unknown;
		int attempts_ = 0;
		int countdown_ = 0;
		bool offObserved_ = false;
	};

	inline constexpr const char* kOverlayKeys[kAnchorCount] = {
		"imguivrhelper.panel.hmd",
		"imguivrhelper.panel.controller",
	};
	inline constexpr const char* kOverlayName = "ImGuiVRHelper Panel";

	inline constexpr const char* kStatusOff = "off";
	inline constexpr const char* kStatusActive = "hosted by the runtime overlay layer";
	inline constexpr const char* kStatusReady = "ready; an open menu is hosted";
	inline constexpr const char* kStatusConfirming =
		"waiting for the runtime to confirm the menu layer";
	inline constexpr const char* kStatusRetrying =
		"in-scene (the runtime refused the menu layer; retrying)";
	inline constexpr const char* kStatusWaitingForTracking =
		"in-scene (waiting for tracking data)";
	inline constexpr const char* kStatusPreparing = "in-scene (preparing the menu image)";
	inline constexpr const char* kStatusFaulted =
		"in-scene (native overlay faulted; toggle the setting to retry)";
	inline constexpr const char* kStatusHeadless = "in-scene (headless VR driver)";
	inline constexpr const char* kStatusOpenComposite =
		"in-scene (OpenComposite: no verified overlay presentation capability)";
	inline constexpr const char* kStatusProxy =
		"in-scene (the overlay interface is a proxy; no verified runtime implementation)";
	inline constexpr const char* kStatusNoInterface =
		"in-scene (the runtime exposes no overlay interface)";
	inline constexpr const char* kStatusUnknownBackend = "in-scene (unrecognized VR runtime)";
	inline constexpr const char* kStatusRenderDisabled =
		"in-scene (the in-scene render path is disabled)";
	inline constexpr const char* kStatusThreadMismatch =
		"in-scene (the Present and Submit hooks run on different threads)";
	inline constexpr const char* kStatusNoPresentProducer =
		"in-scene (the Present producer has not been identified)";
	inline constexpr const char* kStatusCleanupBlocked =
		"cleanup pending (the runtime interface is unavailable)";
	inline constexpr const char* kStatusCleanupIncomplete =
		"cleanup pending (the runtime did not confirm overlay removal)";
	inline constexpr const char* kStatusCleanupPending =
		"cleanup pending (releasing the previous layer resources)";
	inline constexpr const char* kStatusCleanupOwnershipUnverified =
		"cleanup pending (graphics ownership is no longer verified)";
	inline constexpr const char* kStatusThreadConflict =
		"in-scene (a second thread tried to drive the native overlay)";
	inline constexpr const char* kStatusStalled =
		"in-scene (the runtime stopped receiving menu updates; the native layer may stay visible)";

	struct ImageDesc
	{
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::uint32_t mipLevels = 0;
		std::uint32_t arraySize = 0;
		std::uint32_t sampleCount = 0;
		std::uint32_t format = 0;
		const void* device = nullptr;
	};

	constexpr bool IsCopyCompatible(const ImageDesc& d) noexcept
	{
		return d.width != 0 && d.height != 0 && d.mipLevels == 1 && d.arraySize == 1 &&
		       d.sampleCount == 1 && d.format != 0 && d.device != nullptr;
	}

	constexpr bool NeedsRecreate(const ImageDesc& current, const ImageDesc& source) noexcept
	{
		return current.width != source.width || current.height != source.height ||
		       current.format != source.format || current.mipLevels != source.mipLevels ||
		       current.arraySize != source.arraySize ||
		       current.sampleCount != source.sampleCount || current.device != source.device;
	}

	struct PreparedImage
	{
		bool ok = false;
		bool transient = true;
		bool deviceChanged = false;
		const void* texture = nullptr;
		std::uint64_t generation = 0;
		ImageDesc desc{};
	};

	/// Runtime overlay operations. Only the graphics owner calls these and only
	/// while Available() is true; error values are VROverlayError (0 == success).
	struct IOverlayApi
	{
		virtual ~IOverlayApi() = default;
		virtual bool Available() const = 0;
		virtual int CreateOverlay(const char* key, const char* name, std::uint64_t& outHandle) = 0;
		virtual int DestroyOverlay(std::uint64_t handle) = 0;
		virtual int ShowOverlay(std::uint64_t handle) = 0;
		virtual int HideOverlay(std::uint64_t handle) = 0;
		virtual int ClearOverlayTexture(std::uint64_t handle) = 0;
		virtual int SetOverlayTexture(std::uint64_t handle, const void* texture) = 0;
		virtual int SetOverlayWidthInMeters(std::uint64_t handle, float width) = 0;
		virtual int SetOverlayTransformAbsolute(std::uint64_t handle, int origin,
			const float transform[3][4]) = 0;
		virtual const char* ErrorName(int error) const = 0;
	};

	struct IFrameSource
	{
		virtual ~IFrameSource() = default;
		virtual bool Prepare(std::uint32_t focusedClient, PreparedImage& out,
			const char*& reason) = 0;
	};

	struct ISink
	{
		virtual ~ISink() = default;
		virtual void Log(LogLevel level, const std::string& message) = 0;
	};

	struct AnchorPlacement
	{
		bool placed = false;
		float transform[3][4]{};
	};

	struct TickInput
	{
		bool requested = false;
		bool renderPathDisabled = false;
		bool presentProducerEstablished = false;
		bool presentProducerMatches = false;
		Backend backend = Backend::Unknown;
		std::uint32_t focusedClient = 0;
		std::uint64_t configGeneration = 0;
		std::uint32_t requestedMask = 0;
		AnchorPlacement anchor[kAnchorCount];
		int origin = 1;  // vr::TrackingUniverseStanding
		float widthMeters = 1.0f;
	};

	enum class TickAccess : std::uint8_t
	{
		Refused,
		Blocked,
		Idle,
		Authorized,
	};

	/// Ownership facts available from atomics alone. No runtime, frame or shared
	/// game state may be read before AuthorizeOwnerTick returns Authorized.
	struct OwnershipObservation
	{
		bool submitOwner = false;
		bool presentEstablished = false;
		bool presentMatches = false;
		bool ownershipConflict = false;
		const char* blockedStatus = kStatusNoPresentProducer;
	};

	constexpr TickAccess AuthorizeOwnerTick(const OwnershipObservation& o) noexcept
	{
		if (!o.submitOwner)
			return TickAccess::Refused;
		if (o.ownershipConflict || !o.presentEstablished || !o.presentMatches)
			return TickAccess::Blocked;
		return TickAccess::Authorized;
	}

	/// Establishes the single submit owner and the Present producer.
	class ThreadGate
	{
	public:
		explicit ThreadGate(ISink& sink) : sink_(sink) {}

		bool EnterSubmit(std::uint64_t thread)
		{
			if (thread == 0)
				return false;
			std::uint64_t owner = submitOwner_.load(std::memory_order_acquire);
			if (owner == 0 &&
				submitOwner_.compare_exchange_strong(owner, thread,
					std::memory_order_acq_rel)) {
				return true;
			}
			if (owner == thread)
				return true;
			foreignSeen_.store(true, std::memory_order_relaxed);
			if (!foreignLogged_.exchange(true, std::memory_order_relaxed)) {
				sink_.Log(LogLevel::Error,
					"RuntimeOverlay: a second thread tried to drive the overlay; refused");
			}
			return false;
		}

		void EnterPresent(std::uint64_t thread)
		{
			if (thread == 0)
				return;
			std::uint64_t expected = 0;
			if (presentOwner_.compare_exchange_strong(expected, thread,
					std::memory_order_acq_rel)) {
				return;
			}
			if (expected != thread &&
				!presentMigrated_.exchange(true, std::memory_order_acq_rel) &&
				!migrationLogged_.exchange(true, std::memory_order_relaxed)) {
				sink_.Log(LogLevel::Error,
					"RuntimeOverlay: the Present producer moved threads; native hosting is "
					"invalidated");
			}
		}

		bool PresentEstablished() const
		{
			return presentOwner_.load(std::memory_order_acquire) != 0;
		}

		bool PresentOwns(std::uint64_t thread) const
		{
			return thread != 0 &&
			       presentOwner_.load(std::memory_order_acquire) == thread &&
			       !presentMigrated_.load(std::memory_order_acquire);
		}

		bool ForeignThreadSeen() const { return foreignSeen_.load(std::memory_order_relaxed); }
		bool PresentMigrated() const { return presentMigrated_.load(std::memory_order_relaxed); }

		/// Refuses foreign threads and blocks the owner while graphics ownership is
		/// unverified. Only atomics are read here.
		TickAccess EnterOwnerTick(std::uint64_t thread, OwnershipObservation& out)
		{
			if (!EnterSubmit(thread)) {
				out = OwnershipObservation{};
				return TickAccess::Refused;
			}
			const bool migrated = PresentMigrated();
			const bool foreign = ForeignThreadSeen();
			out.submitOwner = true;
			out.presentEstablished = PresentEstablished();
			out.presentMatches = PresentOwns(thread);
			out.ownershipConflict = migrated || foreign;
			if (migrated)
				out.blockedStatus = kStatusThreadMismatch;
			else if (foreign)
				out.blockedStatus = kStatusThreadConflict;
			else if (!out.presentEstablished)
				out.blockedStatus = kStatusNoPresentProducer;
			else
				out.blockedStatus = kStatusThreadMismatch;
			return AuthorizeOwnerTick(out);
		}

	private:
		ISink& sink_;
		std::atomic<std::uint64_t> submitOwner_{ 0 };
		std::atomic<std::uint64_t> presentOwner_{ 0 };
		std::atomic<bool> presentMigrated_{ false };
		std::atomic<bool> foreignSeen_{ false };
		std::atomic<bool> foreignLogged_{ false };
		std::atomic<bool> migrationLogged_{ false };
	};

	struct OwnerTickWork
	{
		std::function<bool()> requested;
		std::function<bool()> needsOwnerWork;
		std::function<void()> run;
		std::function<void(const char*)> standDown;
	};

	/// Production dispatch seam. Refused and blocked ticks invoke no runtime,
	/// frame, source or shared-state callback; authorized idle ticks do not run.
	inline TickAccess RunOwnerTick(const OwnershipObservation& observation, OwnerTickWork& work)
	{
		const auto access = AuthorizeOwnerTick(observation);
		switch (access) {
		case TickAccess::Refused:
			return access;
		case TickAccess::Blocked:
			if (work.standDown)
				work.standDown(observation.blockedStatus);
			return access;
		case TickAccess::Idle:
		case TickAccess::Authorized:
			break;
		}
		const bool requested = work.requested && work.requested();
		const bool needsWork = work.needsOwnerWork && work.needsOwnerWork();
		if (!requested && !needsWork)
			return TickAccess::Idle;
		if (work.run)
			work.run();
		return TickAccess::Authorized;
	}

	inline bool HostingObservable(bool hosting, bool stalled, bool presentMigrated,
		bool foreignSubmit) noexcept
	{
		return hosting && !stalled && !presentMigrated && !foreignSubmit;
	}

	inline const char* ObservedStatus(const char* hostStatus, bool stalled, bool presentMigrated,
		bool foreignSubmit, bool cleanupPending, bool handlesHeld) noexcept
	{
		if (stalled)
			return kStatusStalled;
		if (presentMigrated || foreignSubmit) {
			if (cleanupPending || handlesHeld)
				return kStatusCleanupOwnershipUnverified;
			return presentMigrated ? kStatusThreadMismatch : kStatusThreadConflict;
		}
		return hostStatus;
	}

	struct FrameIdentity
	{
		std::uint32_t client = 0;
		std::uint64_t config = 0;
		int origin = 0;
		std::uint32_t mask = 0;
		std::uint64_t image = 0;

		bool operator==(const FrameIdentity& other) const
		{
			return client == other.client && config == other.config &&
			       origin == other.origin && mask == other.mask && image == other.image;
		}
	};

	class Host
	{
	public:
		Host(IOverlayApi& api, IFrameSource& source, ISink& sink) :
			api_(api), source_(source), sink_(sink) {}

		/// Called only by the established submit owner. Any thread may request a
		/// fault; the owner consumes it and alone reads or writes host state.
		void OwnerTick(const TickInput& in);

		/// Called by the owner when Present ownership cannot be verified. Masks
		/// hosting immediately and retains handles without touching the runtime.
		void OwnerBlocked(const char* status)
		{
			ResetActivationState();
			if (cleanupPending_ || HasHandles()) {
				cleanupPending_ = true;
				SetState(State::CleaningUp, kStatusCleanupOwnershipUnverified);
				return;
			}
			SetState(State::Standing, status);
		}

		void RequestFault(const char* reason)
		{
			const char* expected = nullptr;
			faultReason_.compare_exchange_strong(expected,
				reason ? reason : "tick failed", std::memory_order_acq_rel);
			hosting_.store(false, std::memory_order_release);
			status_.store(kStatusFaulted, std::memory_order_relaxed);
		}

		/// Owner-only: false lets the adapter skip all runtime access while idle.
		bool NeedsOwnerWork() const
		{
			return cleanupPending_ || faulted_ || HasHandles() ||
			       faultReason_.load(std::memory_order_acquire) != nullptr;
		}

		/// Owner-only: true when the tick can reach the runtime at all.
		bool NeedsRuntime() const { return cleanupPending_ || HasHandles(); }

		State GetState() const { return state_.load(std::memory_order_relaxed); }

		/// An unconsumed fault request, from any thread, vetoes visible hosting at
		/// once: the owner must not re-promote a panel it was interrupted from.
		bool Hosting() const
		{
			return hosting_.load(std::memory_order_relaxed) &&
			       faultReason_.load(std::memory_order_acquire) == nullptr;
		}

		bool CleanupPending() const { return cleanupObs_.load(std::memory_order_relaxed); }
		bool HoldsHandles() const { return handlesObs_.load(std::memory_order_relaxed); }

		/// A pending fault request outranks any status the owner wrote mid-tick.
		const char* StatusText() const
		{
			if (faultReason_.load(std::memory_order_acquire) != nullptr)
				return kStatusFaulted;
			return status_.load(std::memory_order_relaxed);
		}

	private:
		struct Slot
		{
			std::uint64_t handle = kInvalidHandle;
			bool shown = false;
			bool hideFailureLogged = false;
			bool clearFailureLogged = false;
			bool destroyFailureLogged = false;
		};

		void ConsumeFaultRequest();
		bool AllRequestedPlaced(const TickInput& in) const;
		bool ApplyFrame(const TickInput& in, const PreparedImage& image);
		bool ShowRequested(std::uint32_t mask);
		bool HideAll();
		bool RunCleanup();
		bool RuntimeLost();
		bool RetireShowing();
		bool HideForRetry(const char* nextStatus);
		void Failure(const std::string& reason);
		void StandDown(const char* status);
		void ResetActivationState();
		void ClearFault();
		bool HasHandles() const;
		void SetState(State state, const char* status);
		void LogOnce(bool& flag, LogLevel level, const std::string& message);
		std::string Describe(const char* operation, int error) const;

		IOverlayApi& api_;
		IFrameSource& source_;
		ISink& sink_;

		Slot slots_[kAnchorCount];
		bool cleanupPending_ = false;
		bool active_ = false;
		bool faulted_ = false;
		bool offObserved_ = false;
		int confirmStreak_ = 0;
		int applyFailures_ = 0;

		FrameIdentity pending_{};
		FrameIdentity token_{};

		std::atomic<const char*> faultReason_{ nullptr };
		std::atomic<State> state_{ State::Disabled };
		std::atomic<const char*> status_{ kStatusOff };
		std::atomic<bool> hosting_{ false };
		std::atomic<bool> cleanupObs_{ false };
		std::atomic<bool> handlesObs_{ false };
	};

	inline void Host::OwnerTick(const TickInput& in)
	{
		ConsumeFaultRequest();

		if (cleanupPending_ && !HasHandles())
			cleanupPending_ = false;
		if (cleanupPending_) {
			if (!in.requested)
				offObserved_ = true;
			ResetActivationState();
			if (!api_.Available()) {
				SetState(State::CleaningUp, kStatusCleanupBlocked);
				return;
			}
			if (!RunCleanup()) {
				SetState(State::CleaningUp, kStatusCleanupIncomplete);
				return;
			}
			ResetActivationState();
			if (offObserved_) {
				offObserved_ = false;
				ClearFault();
			}
			if (!in.requested) {
				SetState(State::Disabled, kStatusOff);
				return;
			}
		}

		if (!in.requested) {
			offObserved_ = true;
			ResetActivationState();
			if (!HasHandles()) {
				offObserved_ = false;
				ClearFault();
				SetState(State::Disabled, kStatusOff);
				return;
			}
			if (!api_.Available()) {
				cleanupPending_ = true;
				SetState(State::CleaningUp, kStatusCleanupBlocked);
				return;
			}
			if (!RunCleanup()) {
				SetState(State::CleaningUp, kStatusCleanupIncomplete);
				return;
			}
			offObserved_ = false;
			ClearFault();
			SetState(State::Disabled, kStatusOff);
			return;
		}

		if (faulted_) {
			SetState(State::Standing, kStatusFaulted);
			return;
		}

		if (in.renderPathDisabled) {
			StandDown(kStatusRenderDisabled);
			return;
		}

		if (!in.presentProducerEstablished) {
			StandDown(kStatusNoPresentProducer);
			return;
		}

		if (!in.presentProducerMatches) {
			StandDown(kStatusThreadMismatch);
			return;
		}

		switch (in.backend) {
		case Backend::SteamVR:
			break;
		case Backend::Headless:
			StandDown(kStatusHeadless);
			return;
		case Backend::OpenComposite:
			StandDown(kStatusOpenComposite);
			return;
		case Backend::Proxy:
			StandDown(kStatusProxy);
			return;
		case Backend::Unsupported:
			StandDown(kStatusNoInterface);
			return;
		case Backend::Unknown:
			StandDown(kStatusUnknownBackend);
			return;
		}

		if (!api_.Available()) {
			StandDown(kStatusNoInterface);
			return;
		}

		if (in.focusedClient == 0 || in.requestedMask == 0) {
			if (!HideForRetry(kStatusReady))
				return;
			return;
		}

		if (!AllRequestedPlaced(in)) {
			if (!HideForRetry(kStatusWaitingForTracking))
				return;
			return;
		}

		if (active_ && (in.focusedClient != token_.client ||
						   in.configGeneration != token_.config || in.origin != token_.origin ||
						   in.requestedMask != token_.mask)) {
			if (!RetireShowing())
				return;
		}

		PreparedImage image;
		const char* reason = nullptr;
		if (!source_.Prepare(in.focusedClient, image, reason)) {
			if (image.transient) {
				HideForRetry(reason ? reason : kStatusPreparing);
			} else {
				Failure(reason ? reason : kStatusPreparing);
			}
			return;
		}

		if (image.deviceChanged) {
			ResetActivationState();
			if (!api_.Available()) {
				if (HasHandles()) {
					cleanupPending_ = true;
					SetState(State::CleaningUp, kStatusCleanupBlocked);
				} else {
					SetState(State::Standing, kStatusNoInterface);
				}
				return;
			}
			const bool hidden = HideAll();
			cleanupPending_ = true;
			SetState(State::CleaningUp,
				hidden ? kStatusCleanupPending : kStatusCleanupIncomplete);
			return;
		}

		if (active_ && image.generation != token_.image) {
			if (!RetireShowing())
				return;
		}

		const FrameIdentity next{ in.focusedClient, in.configGeneration, in.origin,
			in.requestedMask, image.generation };
		if (confirmStreak_ > 0 && !(pending_ == next))
			confirmStreak_ = 0;
		if (confirmStreak_ == 0)
			pending_ = next;

		if (!ApplyFrame(in, image))
			return;

		confirmStreak_ = confirmStreak_ + 1 < kConfirmFrames ? confirmStreak_ + 1 : kConfirmFrames;
		if (confirmStreak_ < kConfirmFrames) {
			SetState(State::Requesting, kStatusConfirming);
			return;
		}

		if (!ShowRequested(in.requestedMask))
			return;

		if (faultReason_.load(std::memory_order_acquire) != nullptr) {
			ResetActivationState();
			SetState(State::Standing, kStatusFaulted);
			return;
		}

		active_ = true;
		token_ = next;
		applyFailures_ = 0;
		hosting_.store(true, std::memory_order_relaxed);
		SetState(State::Active, kStatusActive);
	}

	inline void Host::ConsumeFaultRequest()
	{
		const char* reason = faultReason_.exchange(nullptr, std::memory_order_acq_rel);
		if (!reason)
			return;
		faulted_ = true;
		cleanupPending_ = true;
		ResetActivationState();
		sink_.Log(LogLevel::Error, std::string("RuntimeOverlay: ") + reason +
									   "; native hosting stays off until the setting is toggled");
		SetState(State::Standing, kStatusFaulted);
	}

	inline bool Host::AllRequestedPlaced(const TickInput& in) const
	{
		for (std::uint32_t i = 0; i < kAnchorCount; ++i) {
			if ((in.requestedMask & (1u << i)) != 0 && !in.anchor[i].placed)
				return false;
		}
		return true;
	}

	inline bool Host::ApplyFrame(const TickInput& in, const PreparedImage& image)
	{
		if (!api_.Available())
			return RuntimeLost();
		for (std::uint32_t i = 0; i < kAnchorCount; ++i) {
			if ((in.requestedMask & (1u << i)) == 0)
				continue;
			auto& slot = slots_[i];
			if (slot.handle == kInvalidHandle) {
				if (const int err = api_.CreateOverlay(kOverlayKeys[i], kOverlayName,
						slot.handle);
					err != 0) {
					Failure(Describe("CreateOverlay", err));
					return false;
				}
			}
			if (const int err = api_.SetOverlayWidthInMeters(slot.handle, in.widthMeters);
				err != 0) {
				Failure(Describe("SetOverlayWidthInMeters", err));
				return false;
			}
			if (const int err = api_.SetOverlayTransformAbsolute(slot.handle, in.origin,
					in.anchor[i].transform);
				err != 0) {
				Failure(Describe("SetOverlayTransformAbsolute", err));
				return false;
			}
			if (const int err = api_.SetOverlayTexture(slot.handle, image.texture);
				err != 0) {
				Failure(Describe("SetOverlayTexture", err));
				return false;
			}
		}
		return true;
	}

	inline bool Host::ShowRequested(std::uint32_t mask)
	{
		if (!api_.Available())
			return RuntimeLost();
		for (std::uint32_t i = 0; i < kAnchorCount; ++i) {
			auto& slot = slots_[i];
			if ((mask & (1u << i)) == 0) {
				if (slot.handle != kInvalidHandle && slot.shown) {
					if (const int err = api_.HideOverlay(slot.handle); err != 0) {
						Failure(Describe("HideOverlay", err));
						return false;
					}
					slot.shown = false;
				}
				continue;
			}
			if (slot.shown)
				continue;
			if (const int err = api_.ShowOverlay(slot.handle); err != 0) {
				Failure(Describe("ShowOverlay", err));
				return false;
			}
			slot.shown = true;
		}
		return true;
	}

	inline bool Host::HideAll()
	{
		bool ok = true;
		for (auto& slot : slots_) {
			if (slot.handle == kInvalidHandle || !slot.shown)
				continue;
			if (const int err = api_.HideOverlay(slot.handle); err != 0) {
				ok = false;
				LogOnce(slot.hideFailureLogged, LogLevel::Warn, Describe("HideOverlay", err));
			} else {
				slot.shown = false;
				slot.hideFailureLogged = false;
			}
		}
		return ok;
	}

	inline bool Host::RunCleanup()
	{
		bool complete = true;
		bool hadHandles = false;
		for (auto& slot : slots_) {
			if (slot.handle == kInvalidHandle)
				continue;
			hadHandles = true;
			if (slot.shown) {
				if (const int err = api_.HideOverlay(slot.handle); err != 0) {
					complete = false;
					LogOnce(slot.hideFailureLogged, LogLevel::Warn,
						Describe("HideOverlay", err));
					continue;
				}
				slot.shown = false;
			}
			if (const int err = api_.ClearOverlayTexture(slot.handle); err != 0) {
				complete = false;
				LogOnce(slot.clearFailureLogged, LogLevel::Warn,
					Describe("ClearOverlayTexture", err));
				continue;
			}
			if (const int err = api_.DestroyOverlay(slot.handle); err != 0) {
				complete = false;
				LogOnce(slot.destroyFailureLogged, LogLevel::Warn,
					Describe("DestroyOverlay", err));
				continue;
			}
			slot = Slot{};
		}
		cleanupPending_ = !complete;
		if (complete && hadHandles)
			sink_.Log(LogLevel::Info, "RuntimeOverlay: overlay handles released");
		return complete;
	}

	inline bool Host::RuntimeLost()
	{
		ResetActivationState();
		if (HasHandles())
			cleanupPending_ = true;
		SetState(cleanupPending_ ? State::CleaningUp : State::Standing,
			cleanupPending_ ? kStatusCleanupBlocked : kStatusNoInterface);
		return false;
	}

	inline bool Host::RetireShowing()
	{
		ResetActivationState();
		if (!api_.Available()) {
			if (HasHandles())
				cleanupPending_ = true;
			SetState(cleanupPending_ ? State::CleaningUp : State::Standing,
				cleanupPending_ ? kStatusCleanupBlocked : kStatusNoInterface);
			return false;
		}
		if (!HideAll()) {
			cleanupPending_ = true;
			SetState(State::CleaningUp, kStatusCleanupIncomplete);
			return false;
		}
		return true;
	}

	inline bool Host::HideForRetry(const char* nextStatus)
	{
		if (!RetireShowing())
			return false;
		SetState(State::Requesting, nextStatus);
		return true;
	}

	inline void Host::Failure(const std::string& reason)
	{
		sink_.Log(LogLevel::Warn,
			"RuntimeOverlay: " + reason + "; keeping the in-scene menu");
		ResetActivationState();
		if (!api_.Available()) {
			if (HasHandles())
				cleanupPending_ = true;
		} else if (!HideAll()) {
			cleanupPending_ = true;
		}

		if (++applyFailures_ < kMaxApplyFailures) {
			SetState(cleanupPending_ ? State::CleaningUp : State::Requesting,
				cleanupPending_ ? kStatusCleanupIncomplete : (api_.Available() ? kStatusRetrying : kStatusNoInterface));
			return;
		}
		faulted_ = true;
		cleanupPending_ = true;
		sink_.Log(LogLevel::Error,
			"RuntimeOverlay: the runtime refused the menu layer repeatedly; native hosting "
			"stays off until the setting is toggled");
		SetState(State::Standing, kStatusFaulted);
	}

	inline void Host::StandDown(const char* status)
	{
		ResetActivationState();
		if (!api_.Available()) {
			if (HasHandles())
				cleanupPending_ = true;
			SetState(cleanupPending_ ? State::CleaningUp : State::Standing,
				cleanupPending_ ? kStatusCleanupBlocked : status);
			return;
		}
		if (!HideAll()) {
			cleanupPending_ = true;
			SetState(State::CleaningUp, kStatusCleanupIncomplete);
			return;
		}
		if (HasHandles())
			cleanupPending_ = true;
		SetState(cleanupPending_ ? State::CleaningUp : State::Standing, status);
	}

	inline void Host::ResetActivationState()
	{
		active_ = false;
		confirmStreak_ = 0;
		pending_ = FrameIdentity{};
		token_ = FrameIdentity{};
		hosting_.store(false, std::memory_order_relaxed);
	}

	inline void Host::ClearFault()
	{
		faulted_ = false;
		applyFailures_ = 0;
	}

	inline bool Host::HasHandles() const
	{
		for (const auto& slot : slots_) {
			if (slot.handle != kInvalidHandle)
				return true;
		}
		return false;
	}

	inline void Host::SetState(State state, const char* status)
	{
		handlesObs_.store(HasHandles(), std::memory_order_relaxed);
		state_.store(state, std::memory_order_relaxed);
		status_.store(status, std::memory_order_relaxed);
		cleanupObs_.store(cleanupPending_, std::memory_order_relaxed);
	}

	inline void Host::LogOnce(bool& flag, LogLevel level, const std::string& message)
	{
		if (flag)
			return;
		flag = true;
		sink_.Log(level, "RuntimeOverlay: " + message);
	}

	inline std::string Host::Describe(const char* operation, int error) const
	{
		const char* name = api_.ErrorName(error);
		return std::string(operation) + " failed (" + (name ? name : "error") + ")";
	}
}
