// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include "internal/RuntimeOverlayCore.h"
#include "internal/RuntimeOverlayProvenance.h"

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <string>
#include <utility>
#include <vector>

using namespace ImGuiVRHelper::RuntimeOverlay;
using namespace ImGuiVRHelper::RuntimeOverlay::Core;

namespace
{
	constexpr std::uint64_t kOwnerThread = 42;

	struct MockApi final : IOverlayApi
	{
		bool available = true;
		std::uint64_t nextHandle = 101;
		std::function<int(const std::string&, std::uint64_t)> fail;
		std::vector<std::string> calls;
		std::vector<std::string> unavailableCalls;

		int Record(const char* operation, std::uint64_t handle)
		{
			if (!available) {
				unavailableCalls.emplace_back(operation);
				return 3;
			}
			calls.emplace_back(operation);
			return fail ? fail(operation, handle) : 0;
		}

		int Count(const char* operation) const
		{
			int count = 0;
			for (const auto& call : calls) {
				if (call == operation)
					++count;
			}
			return count;
		}

		bool Available() const override { return available; }

		int CreateOverlay(const char*, const char*, std::uint64_t& out) override
		{
			if (!available) {
				unavailableCalls.emplace_back("CreateOverlay");
				return 3;
			}
			calls.emplace_back("CreateOverlay");
			if (fail) {
				const int error = fail("CreateOverlay", 0);
				if (error != 0)
					return error;
			}
			out = nextHandle++;
			return 0;
		}

		int DestroyOverlay(std::uint64_t handle) override
		{
			return Record("DestroyOverlay", handle);
		}
		int ShowOverlay(std::uint64_t handle) override { return Record("ShowOverlay", handle); }
		int HideOverlay(std::uint64_t handle) override { return Record("HideOverlay", handle); }
		int ClearOverlayTexture(std::uint64_t handle) override
		{
			return Record("ClearOverlayTexture", handle);
		}
		int SetOverlayTexture(std::uint64_t handle, const void*) override
		{
			return Record("SetOverlayTexture", handle);
		}
		int SetOverlayWidthInMeters(std::uint64_t handle, float) override
		{
			return Record("SetOverlayWidthInMeters", handle);
		}
		int SetOverlayTransformAbsolute(std::uint64_t handle, int,
			const float[3][4]) override
		{
			return Record("SetOverlayTransformAbsolute", handle);
		}
		const char* ErrorName(int) const override { return "mock error"; }
	};

	struct MockSource final : IFrameSource
	{
		int prepareCalls = 0;
		bool result = true;
		bool transient = true;
		bool deviceChanged = false;
		std::uint64_t generation = 1;
		const void* texture = reinterpret_cast<const void*>(0x1234);

		bool Prepare(std::uint32_t, PreparedImage& out, const char*& reason) override
		{
			++prepareCalls;
			out = PreparedImage{};
			out.transient = transient;
			out.deviceChanged = deviceChanged;
			if (!result) {
				reason = "mock preparation failed";
				return false;
			}
			out.ok = true;
			out.texture = texture;
			out.generation = generation;
			return true;
		}
	};

	struct CaptureSink final : ISink
	{
		std::vector<std::pair<LogLevel, std::string>> entries;
		void Log(LogLevel level, const std::string& message) override
		{
			entries.emplace_back(level, message);
		}
	};

	struct Harness
	{
		MockApi api;
		MockSource source;
		CaptureSink sink;
		ThreadGate gate{ sink };
		Host host{ api, source, sink };

		bool Submit(const TickInput& in, std::uint64_t thread = kOwnerThread)
		{
			OwnershipObservation observation;
			if (gate.EnterOwnerTick(thread, observation) == TickAccess::Refused)
				return false;
			TickInput authorized = in;
			authorized.presentProducerEstablished = observation.presentEstablished;
			authorized.presentProducerMatches = observation.presentMatches;
			OwnerTickWork work;
			work.standDown = [&](const char* status) { host.OwnerBlocked(status); };
			work.requested = [&] { return authorized.requested; };
			work.needsOwnerWork = [&] { return host.NeedsOwnerWork(); };
			work.run = [&] { host.OwnerTick(authorized); };
			RunOwnerTick(observation, work);
			return true;
		}

		void Present(std::uint64_t thread = kOwnerThread) { gate.EnterPresent(thread); }
		void Tick(const TickInput& in)
		{
			Present();
			Submit(in);
		}
	};

	TickInput SteamInput(std::uint32_t client = 7, std::uint32_t mask = 1u << kHmdAnchor)
	{
		TickInput in;
		in.requested = true;
		in.backend = Backend::SteamVR;
		in.focusedClient = client;
		in.requestedMask = mask;
		in.configGeneration = 1;
		in.origin = 1;
		in.widthMeters = 1.0f;
		for (std::uint32_t i = 0; i < kAnchorCount; ++i) {
			in.anchor[i].placed = (mask & (1u << i)) != 0;
			in.anchor[i].transform[2][3] = static_cast<float>(i);
		}
		return in;
	}

	void Activate(Harness& h, const TickInput& in)
	{
		h.Tick(in);
		h.Tick(in);
		REQUIRE(h.host.Hosting());
		REQUIRE(h.host.GetState() == State::Active);
	}

	void ActivateWithTextures(Harness& h, const TickInput& in)
	{
		Activate(h, in);
		h.api.fail = [](const std::string& op, std::uint64_t) {
			return op == "SetOverlayTexture" ? 7 : 0;
		};
		for (int i = 0; i < kMaxApplyFailures; ++i)
			h.Tick(in);
		REQUIRE(h.host.StatusText() == std::string(kStatusFaulted));
		REQUIRE(h.host.CleanupPending());
	}
}

TEST_CASE("ThreadGate admits one submit owner", "[runtime-overlay]")
{
	CaptureSink sink;
	ThreadGate gate{ sink };
	CHECK(gate.EnterSubmit(7));
	CHECK(gate.EnterSubmit(7));
	CHECK_FALSE(gate.EnterSubmit(8));
	CHECK(gate.ForeignThreadSeen());
	CHECK(gate.EnterSubmit(7));
}

TEST_CASE("ThreadGate requires an established, unchanged Present producer",
	"[runtime-overlay]")
{
	CaptureSink sink;
	ThreadGate gate{ sink };
	CHECK_FALSE(gate.PresentEstablished());
	CHECK_FALSE(gate.PresentOwns(0));
	CHECK_FALSE(gate.PresentOwns(42));

	gate.EnterPresent(42);
	CHECK(gate.PresentEstablished());
	CHECK(gate.PresentOwns(42));
	CHECK_FALSE(gate.PresentOwns(43));

	gate.EnterPresent(43);
	CHECK(gate.PresentMigrated());
	CHECK_FALSE(gate.PresentOwns(42));
	CHECK_FALSE(gate.PresentOwns(43));
}

TEST_CASE("SteamVR hosting confirms over two complete frames", "[runtime-overlay]")
{
	Harness h;
	const auto in = SteamInput();
	h.Tick(in);
	CHECK(h.host.GetState() == State::Requesting);
	CHECK_FALSE(h.host.Hosting());
	CHECK(h.api.Count("ShowOverlay") == 0);
	h.Tick(in);
	CHECK(h.host.Hosting());
	CHECK(h.host.GetState() == State::Active);
	CHECK(h.api.Count("CreateOverlay") == 1);
	CHECK(h.api.Count("ShowOverlay") == 1);
	CHECK(h.api.Count("SetOverlayTexture") == 2);
	CHECK(h.api.Count("SetOverlayTransformAbsolute") == 2);
}

TEST_CASE("Every requested anchor must be placed before anything is applied",
	"[runtime-overlay]")
{
	Harness h;
	auto in = SteamInput(7, (1u << kHmdAnchor) | (1u << kControllerAnchor));
	in.anchor[kControllerAnchor].placed = false;
	for (int i = 0; i < 3; ++i)
		h.Tick(in);
	CHECK(h.host.GetState() == State::Requesting);
	CHECK(h.host.StatusText() == std::string(kStatusWaitingForTracking));
	CHECK(h.api.Count("CreateOverlay") == 0);
	CHECK(h.api.Count("SetOverlayTexture") == 0);
}

TEST_CASE("A partial Both frame never suppresses the in-scene menu", "[runtime-overlay]")
{
	Harness h;
	auto in = SteamInput(7, (1u << kHmdAnchor) | (1u << kControllerAnchor));
	h.api.fail = [](const std::string& op, std::uint64_t handle) {
		return op == "ShowOverlay" && handle == 102 ? 9 : 0;
	};
	h.Tick(in);
	h.Tick(in);
	CHECK_FALSE(h.host.Hosting());
	CHECK(h.host.GetState() != State::Active);
	CHECK(h.api.Count("HideOverlay") >= 1);
}

TEST_CASE("Width, transform and texture failures keep the in-scene menu",
	"[runtime-overlay]")
{
	const char* operations[] = { "SetOverlayWidthInMeters", "SetOverlayTransformAbsolute",
		"SetOverlayTexture" };
	for (const char* operation : operations) {
		Harness h;
		h.api.fail = [operation](const std::string& op, std::uint64_t) {
			return op == operation ? 4 : 0;
		};
		const auto in = SteamInput();
		for (int i = 0; i < 2; ++i)
			h.Tick(in);
		CHECK_FALSE(h.host.Hosting());
		CHECK(h.host.GetState() == State::Requesting);
		CHECK(h.api.Count("ShowOverlay") == 0);
	}
}

TEST_CASE("Turning hosting off releases the handles", "[runtime-overlay]")
{
	Harness h;
	const auto in = SteamInput();
	Activate(h, in);
	auto off = in;
	off.requested = false;
	h.Tick(off);
	CHECK_FALSE(h.host.Hosting());
	CHECK(h.host.GetState() == State::Disabled);
	CHECK_FALSE(h.host.CleanupPending());
	CHECK(h.api.Count("HideOverlay") == 1);
	CHECK(h.api.Count("ClearOverlayTexture") == 1);
	CHECK(h.api.Count("DestroyOverlay") == 1);
}

TEST_CASE("A failed cleanup keeps its handles and is retried", "[runtime-overlay]")
{
	Harness h;
	const auto in = SteamInput();
	Activate(h, in);
	auto off = in;
	off.requested = false;
	h.api.fail = [](const std::string& op, std::uint64_t handle) {
		return op == "DestroyOverlay" && handle == 101 ? 3 : 0;
	};
	h.Tick(off);
	CHECK(h.host.CleanupPending());
	CHECK(h.host.GetState() == State::CleaningUp);
	CHECK(h.api.Count("DestroyOverlay") == 1);
	h.api.fail = nullptr;
	h.Tick(off);
	CHECK_FALSE(h.host.CleanupPending());
	CHECK(h.host.GetState() == State::Disabled);
	CHECK(h.api.Count("DestroyOverlay") == 2);
}

TEST_CASE("Missing interface preserves pending cleanup without claiming success",
	"[runtime-overlay]")
{
	Harness h;
	const auto in = SteamInput();
	Activate(h, in);
	auto off = in;
	off.requested = false;
	h.api.available = false;
	h.Tick(off);
	CHECK(h.host.CleanupPending());
	CHECK(h.host.GetState() == State::CleaningUp);
	CHECK(h.api.Count("DestroyOverlay") == 0);
	CHECK(h.host.StatusText() == std::string(kStatusCleanupBlocked));
	h.api.available = true;
	h.Tick(off);
	CHECK_FALSE(h.host.CleanupPending());
	CHECK(h.host.GetState() == State::Disabled);
}

TEST_CASE("No runtime call is made while the interface is unavailable",
	"[runtime-overlay]")
{
	Harness h;
	const auto in = SteamInput();
	h.api.available = false;
	h.Tick(in);
	CHECK(h.api.unavailableCalls.empty());
	CHECK(h.api.calls.empty());
	CHECK_FALSE(h.host.Hosting());
	CHECK(h.host.GetState() == State::Standing);
	CHECK(h.host.StatusText() == std::string(kStatusNoInterface));

	Harness active;
	Activate(active, in);
	active.api.available = false;
	auto off = in;
	off.requested = false;
	active.Tick(off);
	active.Tick(in);
	CHECK(active.api.unavailableCalls.empty());
	CHECK(active.host.CleanupPending());
	CHECK(active.host.GetState() == State::CleaningUp);
}

TEST_CASE("A client change hides the old panel and reconfirms the new one",
	"[runtime-overlay]")
{
	Harness h;
	const auto first = SteamInput(7);
	Activate(h, first);
	const auto second = SteamInput(8);
	h.Tick(second);
	CHECK_FALSE(h.host.Hosting());
	CHECK(h.host.GetState() == State::Requesting);
	CHECK(h.api.Count("HideOverlay") == 1);
	CHECK(h.api.Count("ShowOverlay") == 1);
	h.Tick(second);
	CHECK(h.host.Hosting());
	CHECK(h.api.Count("ShowOverlay") == 2);
}

TEST_CASE("A configuration or origin change reconfirms placement", "[runtime-overlay]")
{
	Harness h;
	auto in = SteamInput();
	Activate(h, in);
	in.configGeneration = 2;
	h.Tick(in);
	CHECK_FALSE(h.host.Hosting());
	h.Tick(in);
	CHECK(h.host.Hosting());

	in.origin = 0;
	h.Tick(in);
	CHECK_FALSE(h.host.Hosting());
	h.Tick(in);
	CHECK(h.host.Hosting());
}

TEST_CASE("Confirmation binds to the first successful frame's identity",
	"[runtime-overlay]")
{
	Harness h;
	const auto first = SteamInput(7);
	const auto second = SteamInput(8);
	h.Tick(first);
	CHECK(h.host.GetState() == State::Requesting);
	h.Tick(second);
	CHECK_FALSE(h.host.Hosting());
	CHECK(h.api.Count("ShowOverlay") == 0);
	h.Tick(second);
	CHECK(h.host.Hosting());
	CHECK(h.api.Count("ShowOverlay") == 1);
}

TEST_CASE("Configuration, origin and mask changes cannot inherit a confirmation streak",
	"[runtime-overlay]")
{
	auto config = SteamInput();
	config.configGeneration = 2;
	auto origin = SteamInput();
	origin.origin = 0;
	auto mask = SteamInput(7, (1u << kHmdAnchor) | (1u << kControllerAnchor));

	const TickInput changes[] = { config, origin, mask };
	for (const auto& changed : changes) {
		Harness h;
		const auto first = SteamInput();
		h.Tick(first);
		REQUIRE_FALSE(h.host.Hosting());
		h.Tick(changed);
		CHECK_FALSE(h.host.Hosting());
		CHECK(h.api.Count("ShowOverlay") == 0);
		h.Tick(changed);
		CHECK(h.host.Hosting());
		int expectedShows = 0;
		for (std::uint32_t i = 0; i < kAnchorCount; ++i) {
			if ((changed.requestedMask & (1u << i)) != 0)
				++expectedShows;
		}
		CHECK(h.api.Count("ShowOverlay") == expectedShows);
	}
}

TEST_CASE("Unsupported backends never touch the runtime", "[runtime-overlay]")
{
	struct Case
	{
		Backend backend;
		const char* status;
	};
	const Case cases[] = {
		{ Backend::OpenComposite, kStatusOpenComposite },
		{ Backend::Proxy, kStatusProxy },
		{ Backend::Headless, kStatusHeadless },
		{ Backend::Unsupported, kStatusNoInterface },
		{ Backend::Unknown, kStatusUnknownBackend },
	};
	for (const auto& entry : cases) {
		Harness h;
		auto in = SteamInput();
		in.backend = entry.backend;
		for (int i = 0; i < 5; ++i)
			h.Tick(in);
		CHECK(h.api.calls.empty());
		CHECK_FALSE(h.host.Hosting());
		CHECK(h.host.GetState() == State::Standing);
		CHECK(h.host.StatusText() == std::string(entry.status));
	}
}

TEST_CASE("A tick from a second thread is refused", "[runtime-overlay]")
{
	Harness h;
	const auto in = SteamInput();
	Activate(h, in);
	const auto callsBefore = h.api.calls.size();
	CHECK_FALSE(h.Submit(in, 99));
	CHECK(h.gate.ForeignThreadSeen());
	CHECK(h.api.calls.size() == callsBefore);
	CHECK_FALSE(HostingObservable(h.host.Hosting(), false, h.gate.PresentMigrated(),
		h.gate.ForeignThreadSeen()));
	h.Tick(in);
	CHECK_FALSE(h.host.Hosting());
	CHECK(h.host.CleanupPending());
	CHECK(h.api.calls.size() == callsBefore);
}

TEST_CASE("A Present producer on another thread keeps the in-scene menu",
	"[runtime-overlay]")
{
	Harness h;
	const auto in = SteamInput();
	h.Present(99);
	h.Submit(in);
	CHECK(h.api.calls.empty());
	CHECK_FALSE(h.host.Hosting());
	CHECK(h.host.GetState() == State::Standing);
	CHECK(h.host.StatusText() == std::string(kStatusThreadMismatch));
}

TEST_CASE("Hosting waits for an identified Present producer", "[runtime-overlay]")
{
	Harness h;
	const auto in = SteamInput();
	CHECK(h.Submit(in));
	CHECK(h.api.calls.empty());
	CHECK_FALSE(h.host.Hosting());
	CHECK(h.host.StatusText() == std::string(kStatusNoPresentProducer));
	h.Present();
	h.Tick(in);
	h.Tick(in);
	CHECK(h.host.Hosting());
}

TEST_CASE("A Present producer migration invalidates active hosting", "[runtime-overlay]")
{
	Harness h;
	const auto in = SteamInput();
	Activate(h, in);

	h.Present(99);
	CHECK(h.gate.PresentMigrated());
	CHECK_FALSE(h.gate.PresentOwns(kOwnerThread));

	const auto calls = h.api.calls.size();
	h.Submit(in);
	CHECK_FALSE(h.host.Hosting());
	CHECK(h.host.CleanupPending());
	CHECK(h.host.GetState() == State::CleaningUp);
	CHECK(h.host.StatusText() == std::string(kStatusCleanupOwnershipUnverified));
	CHECK(h.api.calls.size() == calls);

	h.Submit(in);
	CHECK(h.host.CleanupPending());
	CHECK(h.host.GetState() == State::CleaningUp);
	CHECK(h.api.calls.size() == calls);
}

TEST_CASE("A layer generation change reconfirms without dropping the handles",
	"[runtime-overlay]")
{
	Harness h;
	const auto in = SteamInput();
	Activate(h, in);
	h.source.generation = 2;
	h.Tick(in);
	CHECK_FALSE(h.host.Hosting());
	CHECK(h.api.Count("DestroyOverlay") == 0);
	h.Tick(in);
	CHECK(h.host.Hosting());
}

TEST_CASE("A failed hide on a layer generation change stops the transaction",
	"[runtime-overlay]")
{
	Harness h;
	const auto in = SteamInput();
	Activate(h, in);
	h.source.generation = 2;
	h.api.fail = [](const std::string& op, std::uint64_t) {
		return op == "HideOverlay" ? 5 : 0;
	};
	h.Tick(in);
	CHECK_FALSE(h.host.Hosting());
	CHECK(h.host.CleanupPending());
	CHECK(h.host.GetState() == State::CleaningUp);
	CHECK(h.api.Count("ShowOverlay") == 1);

	h.api.fail = nullptr;
	h.Tick(in);
	h.Tick(in);
	CHECK(h.host.Hosting());
	CHECK(h.api.Count("ShowOverlay") == 2);
}

TEST_CASE("A failed hide during a device change keeps cleanup pending",
	"[runtime-overlay]")
{
	Harness h;
	const auto in = SteamInput();
	Activate(h, in);
	h.source.deviceChanged = true;
	h.api.fail = [](const std::string& op, std::uint64_t) {
		return op == "HideOverlay" ? 5 : 0;
	};
	h.Tick(in);
	CHECK_FALSE(h.host.Hosting());
	CHECK(h.host.CleanupPending());
	CHECK(h.host.GetState() == State::CleaningUp);
	CHECK(h.api.Count("DestroyOverlay") == 0);

	h.api.fail = nullptr;
	h.source.deviceChanged = false;
	h.Tick(in);
	CHECK_FALSE(h.host.CleanupPending());
	CHECK(h.api.Count("DestroyOverlay") == 1);
}

TEST_CASE("A failed hide during a client change blocks reconfirmation",
	"[runtime-overlay]")
{
	Harness h;
	Activate(h, SteamInput(7));
	h.api.fail = [](const std::string& op, std::uint64_t) {
		return op == "HideOverlay" ? 5 : 0;
	};
	h.Tick(SteamInput(8));
	CHECK_FALSE(h.host.Hosting());
	CHECK(h.host.CleanupPending());
	CHECK(h.host.GetState() == State::CleaningUp);
	CHECK(h.api.Count("ShowOverlay") == 1);

	h.api.fail = nullptr;
	h.Tick(SteamInput(8));
	h.Tick(SteamInput(8));
	CHECK(h.host.Hosting());
	CHECK(h.api.Count("ShowOverlay") == 2);
}

TEST_CASE("A device change releases the old handles before hosting again",
	"[runtime-overlay]")
{
	Harness h;
	const auto in = SteamInput();
	Activate(h, in);
	h.source.deviceChanged = true;
	h.Tick(in);
	CHECK_FALSE(h.host.Hosting());
	CHECK(h.host.CleanupPending());
	CHECK(h.host.GetState() == State::CleaningUp);
	h.source.deviceChanged = false;
	h.Tick(in);
	CHECK_FALSE(h.host.CleanupPending());
	CHECK(h.api.Count("DestroyOverlay") == 1);
	h.Tick(in);
	h.Tick(in);
	CHECK(h.host.Hosting());
}

TEST_CASE("A transient preparation failure does not latch hosting off",
	"[runtime-overlay]")
{
	Harness h;
	h.source.result = false;
	h.source.transient = true;
	const auto in = SteamInput();
	for (int i = 0; i < 5; ++i)
		h.Tick(in);
	CHECK(h.host.GetState() == State::Requesting);
	CHECK_FALSE(h.host.Hosting());
	CHECK(h.host.StatusText() == std::string("mock preparation failed"));
	h.source.result = true;
	Activate(h, in);
}

TEST_CASE("Permanent preparation failures hide the layer and reach the fault latch",
	"[runtime-overlay]")
{
	Harness h;
	const auto in = SteamInput();
	Activate(h, in);
	h.sink.entries.clear();
	const auto textureCalls = h.api.Count("SetOverlayTexture");
	const auto initialPrepareCalls = h.source.prepareCalls;
	h.source.result = false;
	h.source.transient = false;
	for (int i = 0; i < kMaxApplyFailures; ++i) {
		h.Tick(in);
		CHECK_FALSE(h.host.Hosting());
	}
	CHECK(h.api.Count("HideOverlay") >= 1);
	CHECK(h.api.Count("SetOverlayTexture") == textureCalls);
	CHECK(h.source.prepareCalls == initialPrepareCalls + kMaxApplyFailures);
	CHECK(h.host.GetState() == State::Standing);
	CHECK(h.host.StatusText() == std::string(kStatusFaulted));
	CHECK(h.host.CleanupPending());
	REQUIRE_FALSE(h.sink.entries.empty());
	CHECK(h.sink.entries.front().second.find("mock preparation failed") != std::string::npos);

	const auto prepareCalls = h.source.prepareCalls;
	h.source.result = true;
	h.Tick(in);
	h.Tick(in);
	CHECK(h.source.prepareCalls == prepareCalls);
	CHECK_FALSE(h.host.Hosting());
	CHECK_FALSE(h.host.CleanupPending());
	CHECK(h.api.Count("DestroyOverlay") == 1);

	auto off = in;
	off.requested = false;
	h.Tick(off);
	Activate(h, in);
}

TEST_CASE("Repeated refusals latch off, clean up and recover through the toggle",
	"[runtime-overlay]")
{
	Harness h;
	h.api.fail = [](const std::string& op, std::uint64_t) {
		return op == "SetOverlayTexture" ? 7 : 0;
	};
	const auto in = SteamInput();
	for (int i = 0; i < kMaxApplyFailures; ++i)
		h.Tick(in);
	CHECK(h.host.GetState() == State::Standing);
	CHECK(h.host.StatusText() == std::string(kStatusFaulted));
	CHECK(h.host.CleanupPending());

	const auto textureCalls = h.api.Count("SetOverlayTexture");
	h.Tick(in);
	h.Tick(in);
	CHECK(h.api.Count("SetOverlayTexture") == textureCalls);

	auto off = in;
	off.requested = false;
	h.api.fail = nullptr;
	h.Tick(off);
	CHECK_FALSE(h.host.CleanupPending());
	CHECK(h.host.GetState() == State::Disabled);

	Activate(h, in);
	CHECK(h.api.Count("ShowOverlay") >= 1);
}

TEST_CASE("Repeated Show failures on Both anchors reach the fault latch",
	"[runtime-overlay]")
{
	Harness h;
	const auto in = SteamInput(7, (1u << kHmdAnchor) | (1u << kControllerAnchor));
	h.api.fail = [](const std::string& op, std::uint64_t handle) {
		return op == "ShowOverlay" && handle == 102 ? 9 : 0;
	};
	for (int i = 0; i < 2 * (kMaxApplyFailures + 2); ++i)
		h.Tick(in);
	CHECK(h.host.GetState() == State::Standing);
	CHECK(h.host.StatusText() == std::string(kStatusFaulted));
	CHECK_FALSE(h.host.Hosting());
	CHECK(h.api.Count("ShowOverlay") == 2 * kMaxApplyFailures);
}

TEST_CASE("A late success after disable still needs a fresh confirmation",
	"[runtime-overlay]")
{
	Harness h;
	const auto in = SteamInput();
	Activate(h, in);
	auto off = in;
	off.requested = false;
	h.Tick(off);
	REQUIRE(h.host.GetState() == State::Disabled);

	h.Tick(in);
	CHECK_FALSE(h.host.Hosting());
	CHECK(h.host.GetState() == State::Requesting);
	h.Tick(in);
	CHECK(h.host.Hosting());
}

TEST_CASE("A failed hide on menu close schedules cleanup", "[runtime-overlay]")
{
	Harness h;
	auto in = SteamInput();
	Activate(h, in);
	h.api.fail = [](const std::string& op, std::uint64_t) {
		return op == "HideOverlay" ? 5 : 0;
	};
	auto closed = in;
	closed.focusedClient = 0;
	h.Tick(closed);
	CHECK_FALSE(h.host.Hosting());
	CHECK(h.host.CleanupPending());
	CHECK(h.host.GetState() == State::CleaningUp);
	h.api.fail = nullptr;
	h.Tick(closed);
	CHECK_FALSE(h.host.CleanupPending());
	CHECK(h.host.GetState() == State::Requesting);
}

TEST_CASE("A failed texture clear keeps the handle for a later retry", "[runtime-overlay]")
{
	Harness h;
	const auto in = SteamInput();
	Activate(h, in);
	auto off = in;
	off.requested = false;
	h.api.fail = [](const std::string& op, std::uint64_t) {
		return op == "ClearOverlayTexture" ? 6 : 0;
	};
	h.Tick(off);
	CHECK(h.host.CleanupPending());
	CHECK(h.api.Count("DestroyOverlay") == 0);
	h.api.fail = nullptr;
	h.Tick(off);
	CHECK_FALSE(h.host.CleanupPending());
	CHECK(h.host.GetState() == State::Disabled);
}

TEST_CASE("An escaped tick fault is consumed by the owner and still cleans up",
	"[runtime-overlay]")
{
	Harness h;
	const auto in = SteamInput();
	Activate(h, in);
	h.host.RequestFault("tick threw");
	CHECK_FALSE(h.host.Hosting());
	CHECK(h.host.StatusText() == std::string(kStatusFaulted));

	const auto textures = h.api.Count("SetOverlayTexture");
	h.Tick(in);
	CHECK_FALSE(h.host.Hosting());
	CHECK(h.host.GetState() == State::Standing);
	CHECK(h.host.StatusText() == std::string(kStatusFaulted));
	CHECK_FALSE(h.host.CleanupPending());
	CHECK(h.api.Count("SetOverlayTexture") == textures);
	h.Tick(in);
	CHECK(h.host.StatusText() == std::string(kStatusFaulted));
}

TEST_CASE("A fault requested during the Show transaction is observable at once",
	"[runtime-overlay]")
{
	Harness h;
	const auto in = SteamInput();
	h.Tick(in);
	REQUIRE(h.host.GetState() == State::Requesting);

	bool faultedInShow = false;
	h.api.fail = [&](const std::string& op, std::uint64_t) {
		if (op == "ShowOverlay") {
			h.host.RequestFault("mock overlay fault");
			faultedInShow = true;
		}
		return 0;
	};
	h.Tick(in);
	CHECK(faultedInShow);

	CHECK_FALSE(h.host.Hosting());
	CHECK(h.host.GetState() == State::Standing);
	CHECK(h.host.StatusText() == std::string(kStatusFaulted));
	CHECK(h.api.Count("ShowOverlay") == 1);
	CHECK_FALSE(h.host.CleanupPending());
	CHECK(h.host.NeedsOwnerWork());

	h.api.fail = nullptr;
	h.Tick(in);
	CHECK_FALSE(h.host.Hosting());
	CHECK(h.host.GetState() == State::Standing);
	CHECK(h.host.StatusText() == std::string(kStatusFaulted));
	CHECK_FALSE(h.host.CleanupPending());
	CHECK(h.api.Count("DestroyOverlay") == 1);
	CHECK(h.api.Count("ShowOverlay") == 1);
}

TEST_CASE("An off request seen during blocked cleanup permits one new attempt",
	"[runtime-overlay]")
{
	Harness h;
	const auto in = SteamInput();
	ActivateWithTextures(h, in);

	h.api.available = false;
	auto off = in;
	off.requested = false;
	h.Tick(off);
	CHECK(h.host.CleanupPending());
	CHECK(h.host.GetState() == State::CleaningUp);

	h.Tick(in);
	CHECK(h.host.CleanupPending());
	CHECK(h.host.GetState() == State::CleaningUp);
	CHECK(h.api.unavailableCalls.empty());

	h.api.available = true;
	h.api.fail = nullptr;
	h.Tick(in);
	CHECK_FALSE(h.host.CleanupPending());
	CHECK_FALSE(h.host.Hosting());
	h.Tick(in);
	CHECK(h.host.Hosting());
	CHECK(h.api.unavailableCalls.empty());
}

TEST_CASE("An idle disabled host needs no owner work", "[runtime-overlay]")
{
	Harness h;
	CHECK_FALSE(h.host.NeedsOwnerWork());
	CHECK_FALSE(h.host.NeedsRuntime());

	auto idle = SteamInput();
	idle.requested = false;
	h.Tick(idle);
	CHECK(h.host.GetState() == State::Disabled);
	CHECK(h.api.calls.empty());
	CHECK_FALSE(h.host.NeedsOwnerWork());
	CHECK_FALSE(h.host.NeedsRuntime());

	Activate(h, SteamInput());
	CHECK(h.host.NeedsOwnerWork());
	CHECK(h.host.NeedsRuntime());
}

TEST_CASE("Layer copy compatibility and recreation key on the full descriptor",
	"[runtime-overlay]")
{
	ImageDesc d;
	d.width = 1920;
	d.height = 1080;
	d.mipLevels = 1;
	d.arraySize = 1;
	d.sampleCount = 1;
	d.format = 28;
	d.device = reinterpret_cast<const void*>(0x10);
	CHECK(IsCopyCompatible(d));

	auto bad = d;
	bad.mipLevels = 2;
	CHECK_FALSE(IsCopyCompatible(bad));
	bad = d;
	bad.arraySize = 2;
	CHECK_FALSE(IsCopyCompatible(bad));
	bad = d;
	bad.sampleCount = 4;
	CHECK_FALSE(IsCopyCompatible(bad));
	bad = d;
	bad.width = 0;
	CHECK_FALSE(IsCopyCompatible(bad));
	bad = d;
	bad.device = nullptr;
	CHECK_FALSE(IsCopyCompatible(bad));

	CHECK_FALSE(NeedsRecreate(d, d));
	auto changed = d;
	changed.device = reinterpret_cast<const void*>(0x20);
	CHECK(NeedsRecreate(d, changed));
	changed = d;
	changed.format = 10;
	CHECK(NeedsRecreate(d, changed));
	changed = d;
	changed.width = 1280;
	CHECK(NeedsRecreate(d, changed));
}

TEST_CASE("Clean runtime provenance requires the stock location and Valve identity",
	"[runtime-overlay]")
{
	using namespace ImGuiVRHelper::RuntimeOverlay::Provenance;
	const std::vector<std::wstring> roots = {
		L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR"
	};
	const std::wstring game = L"D:\\SteamLibrary\\steamapps\\common\\SkyrimVR\\";
	const ModuleIdentity valve{ true, L"Valve Corporation", L"VR Client" };
	const ModuleIdentity openComposite{ true, L"OpenComposite", L"OpenComposite OpenVR" };
	const ModuleIdentity other{ true, L"Example Corp", L"Runtime Wrapper" };
	const ModuleIdentity unavailable{};

	const std::wstring stock =
		L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\bin\\vrclient_x64.dll";

	CHECK(ClassifyRuntimeModule(stock, roots, game, valve) == Runtime::SteamVR);
	CHECK(ClassifyRuntimeModule(
			  L"C:\\Program Files (x86)\\STEAM\\steamapps\\common\\steamvr\\BIN\\VRCLIENT_X64.DLL",
			  roots, game, valve) == Runtime::SteamVR);

	CHECK(ClassifyRuntimeModule(stock, roots, game, openComposite) == Runtime::Proxy);
	CHECK(ClassifyRuntimeModule(stock, roots, game, other) == Runtime::Proxy);
	CHECK(ClassifyRuntimeModule(stock, roots, game, unavailable) == Runtime::Unknown);

	const std::vector<std::wstring> ocuRoots = { L"C:\\OpenComposite" };
	CHECK(ClassifyRuntimeModule(L"c:\\opencomposite\\bin\\win64\\vrclient_x64.dll", ocuRoots,
			  game, openComposite) == Runtime::Proxy);
	CHECK(ClassifyRuntimeModule(L"c:\\opencomposite\\bin\\win64\\vrclient_x64.dll", ocuRoots,
			  game, valve) == Runtime::Proxy);

	CHECK(ClassifyRuntimeModule(
			  L"D:\\SteamLibrary\\steamapps\\common\\SkyrimVR\\openvr_api.dll", roots, game,
			  unavailable) == Runtime::OpenComposite);
	CHECK(ClassifyRuntimeModule(
			  L"D:\\SteamLibrary\\steamapps\\common\\SkyrimVR\\opencomposite\\overlay.dll",
			  roots, game, unavailable) == Runtime::OpenComposite);
	CHECK(ClassifyRuntimeModule(L"D:\\SteamLibrary\\steamapps\\common\\SkyrimVR\\openvr_api.dll",
			  {}, game) == Runtime::OpenComposite);

	CHECK(ClassifyRuntimeModule(L"D:\\Games\\steamvr-shim\\vrclient_x64.dll", roots, game,
			  valve) == Runtime::Unknown);
	CHECK(ClassifyRuntimeModule(
			  L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\tools\\vrclient_x64.dll",
			  roots, game, valve) == Runtime::Proxy);
	CHECK(ClassifyRuntimeModule(
			  L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\bin\\win64\\wrapper.dll",
			  roots, game, valve) == Runtime::Proxy);
	CHECK(ClassifyRuntimeModule(L"C:\\Somewhere\\runtime.dll", roots, game, valve) ==
		  Runtime::Unknown);
	CHECK(ClassifyRuntimeModule(L"", roots, game, valve) == Runtime::Unknown);
}

TEST_CASE("Win32 version metadata is read for runtime identity", "[runtime-overlay]")
{
	using namespace ImGuiVRHelper::RuntimeOverlay::Provenance;
	wchar_t systemDirectory[MAX_PATH]{};
	REQUIRE(GetSystemDirectoryW(systemDirectory, MAX_PATH) != 0);
	const auto identity =
		ReadModuleIdentity(std::wstring(systemDirectory) + L"\\kernel32.dll");
	CHECK(identity.available);
	CHECK_FALSE(identity.company.empty());
	CHECK_FALSE(IsSteamVrImplementation(identity));
	CHECK_FALSE(ReadModuleIdentity(L"C:\\does\\not\\exist.dll").available);
	CHECK_FALSE(ReadModuleIdentity(L"").available);
}

TEST_CASE("An installed stock SteamVR module matches the runtime identity",
	"[runtime-overlay]")
{
	using namespace ImGuiVRHelper::RuntimeOverlay::Provenance;
	const std::wstring root =
		L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR";
	const std::wstring stock = root + L"\\bin\\vrclient_x64.dll";
	if (GetFileAttributesW(stock.c_str()) == INVALID_FILE_ATTRIBUTES)
		return;
	const auto identity = ReadModuleIdentity(stock);
	CHECK(IsSteamVrImplementation(identity));
	CHECK(ClassifyRuntimeModule(stock, { root }, L"", identity) == Runtime::SteamVR);
}

TEST_CASE("openvrpaths runtime roots are parsed conservatively", "[runtime-overlay]")
{
	using namespace ImGuiVRHelper::RuntimeOverlay::Provenance;
	const auto roots =
		ParseRuntimeRoots(R"({"runtime":["C:\\SteamVR","D:\\SteamVR2"],"config":[]})");
	REQUIRE(roots.size() == 2);
	CHECK(roots[0] == "C:\\SteamVR");
	CHECK(roots[1] == "D:\\SteamVR2");
	CHECK(ParseRuntimeRoots("not json").empty());
	CHECK(ParseRuntimeRoots(R"({"config":[]})").empty());
}

TEST_CASE("Owner dispatch performs no work before ownership is authorized",
	"[runtime-overlay]")
{
	int requested = 0;
	int needsWork = 0;
	int run = 0;
	int standDown = 0;
	OwnerTickWork work;
	work.requested = [&] {
		++requested;
		return true;
	};
	work.needsOwnerWork = [&] {
		++needsWork;
		return false;
	};
	work.run = [&] { ++run; };
	work.standDown = [&](const char*) { ++standDown; };

	CHECK(requested == 0);
	CHECK(needsWork == 0);
	CHECK(run == 0);
	CHECK(standDown == 0);

	const OwnershipObservation refused;
	CHECK(RunOwnerTick(refused, work) == TickAccess::Refused);
	CHECK(requested == 0);
	CHECK(needsWork == 0);
	CHECK(run == 0);
	CHECK(standDown == 0);

	const OwnershipObservation beforePresent{ true, false, false, false,
		kStatusNoPresentProducer };
	CHECK(RunOwnerTick(beforePresent, work) == TickAccess::Blocked);
	CHECK(standDown == 1);
	CHECK(requested == 0);
	CHECK(needsWork == 0);
	CHECK(run == 0);

	const OwnershipObservation mismatch{ true, true, false, false, kStatusThreadMismatch };
	CHECK(RunOwnerTick(mismatch, work) == TickAccess::Blocked);
	CHECK(standDown == 2);
	CHECK(requested == 0);

	const OwnershipObservation migrated{ true, true, false, true, kStatusThreadMismatch };
	CHECK(RunOwnerTick(migrated, work) == TickAccess::Blocked);
	CHECK(standDown == 3);
	CHECK(requested == 0);

	const OwnershipObservation conflict{ true, true, true, true, kStatusThreadConflict };
	CHECK(RunOwnerTick(conflict, work) == TickAccess::Blocked);
	CHECK(standDown == 4);
	CHECK(requested == 0);

	const OwnershipObservation authorized{ true, true, true, false, nullptr };
	CHECK(RunOwnerTick(authorized, work) == TickAccess::Authorized);
	CHECK(requested == 1);
	CHECK(needsWork == 1);
	CHECK(run == 1);
	CHECK(standDown == 4);
}

TEST_CASE("Authorized idle dispatch does not run runtime work", "[runtime-overlay]")
{
	bool requested = false;
	bool needsWork = false;
	int run = 0;
	OwnerTickWork work;
	work.requested = [&] { return requested; };
	work.needsOwnerWork = [&] { return needsWork; };
	work.run = [&] { ++run; };
	const OwnershipObservation authorized{ true, true, true, false, nullptr };

	CHECK(RunOwnerTick(authorized, work) == TickAccess::Idle);
	CHECK(run == 0);

	needsWork = true;
	CHECK(RunOwnerTick(authorized, work) == TickAccess::Authorized);
	CHECK(run == 1);
}

TEST_CASE("Ownership observations mask hosting without another owner tick",
	"[runtime-overlay]")
{
	{
		Harness h;
		Activate(h, SteamInput());
		h.host.RequestFault("tick threw");
		CHECK_FALSE(h.host.Hosting());
		CHECK(h.host.StatusText() == std::string(kStatusFaulted));
	}
	{
		Harness h;
		Activate(h, SteamInput());
		CHECK_FALSE(h.Submit(SteamInput(), 99));
		CHECK_FALSE(HostingObservable(h.host.Hosting(), false, false,
			h.gate.ForeignThreadSeen()));
		CHECK(ObservedStatus(h.host.StatusText(), false, false, true,
				  h.host.CleanupPending(), h.host.HoldsHandles()) ==
			  std::string(kStatusCleanupOwnershipUnverified));
	}
	{
		Harness h;
		Activate(h, SteamInput());
		h.Present(99);
		CHECK_FALSE(HostingObservable(h.host.Hosting(), false,
			h.gate.PresentMigrated(), false));
		CHECK(ObservedStatus(h.host.StatusText(), false, true, false,
				  h.host.CleanupPending(), h.host.HoldsHandles()) ==
			  std::string(kStatusCleanupOwnershipUnverified));
		CHECK_FALSE(HostingObservable(h.host.Hosting(), true, false, false));
	}
}

TEST_CASE("Detection budget retries only after an observed off and on", "[runtime-overlay]")
{
	DetectionBudget budget;
	for (int i = 0; i < kMaxDetectAttempts; ++i) {
		REQUIRE(budget.Next() == ProbeDecision::Probe);
		budget.Record(Backend::Unknown);
		for (int retry = 0; retry < kDetectRetryTicks; ++retry)
			CHECK(budget.Next() == ProbeDecision::Cached);
	}
	CHECK(budget.Next() == ProbeDecision::Exhausted);
	CHECK(budget.Cached() == Backend::Unsupported);
	CHECK(budget.Next() == ProbeDecision::Cached);

	CHECK_FALSE(budget.ObserveSetting(false, false));
	CHECK_FALSE(budget.ObserveSetting(true, true));
	CHECK(budget.Cached() == Backend::Unsupported);

	CHECK_FALSE(budget.ObserveSetting(false, false));
	CHECK(budget.ObserveSetting(true, false));
	CHECK(budget.Cached() == Backend::Unknown);
	CHECK(budget.Attempts() == 0);
	CHECK(budget.Next() == ProbeDecision::Probe);
}
