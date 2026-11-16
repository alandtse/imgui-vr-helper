// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
#include <catch2/catch_test_macros.hpp>

#include "internal/ButtonSources.h"

using ImGuiVRHelper::ButtonSources::Source;
using ImGuiVRHelper::ButtonSources::State;

TEST_CASE("button sources retain a hold until every source releases")
{
	State state;
	REQUIRE(state.Set(Source::Physical, 33, true));
	REQUIRE(state.Set(Source::Poke, 33, true));
	REQUIRE(state.IsDown(33));

	REQUIRE(state.Set(Source::Poke, 33, false));
	CHECK(state.IsDown(33));
	REQUIRE(state.Set(Source::Physical, 33, false));
	CHECK_FALSE(state.IsDown(33));
}

TEST_CASE("button sources keep independent injected and physical state")
{
	State state;
	REQUIRE(state.Set(Source::Physical, 33, true));
	REQUIRE(state.Set(Source::Injected, 33, true));
	REQUIRE(state.Set(Source::Physical, 33, false));
	CHECK(state.IsDown(33));
	REQUIRE(state.Set(Source::Injected, 33, false));
	CHECK_FALSE(state.IsDown(33));
}

TEST_CASE("button sources support repeated poke contact cycles")
{
	State state;
	for (int cycle = 0; cycle < 4; ++cycle) {
		REQUIRE(state.Set(Source::Poke, 33, true));
		CHECK(state.IsDown(33));
		REQUIRE(state.Set(Source::Poke, 33, false));
		CHECK_FALSE(state.IsDown(33));
	}
}

TEST_CASE("button sources reject out of range keys")
{
	State state;
	CHECK_FALSE(state.Set(Source::Poke, State::kKeyCount, true));
	CHECK_FALSE(state.IsDown(State::kKeyCount));
}
