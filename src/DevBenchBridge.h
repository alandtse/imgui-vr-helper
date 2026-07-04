// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2026 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Bridge to the devbench SKSE dev harness (https://github.com/alandtse/devbench):
// registers the helper's input diagnostics and synthetic-input injection as
// devbench MCP/REST tools, so an agent can inspect wand/lease/focus state and
// drive deterministic pointer + button sequences without a headset. Runtime
// no-op when no devbench host is installed.

#pragma once

namespace ImGuiVRHelper::DevBenchBridge
{
	/// Handshake with a devbench host (if any) and register the helper's tools.
	/// Call at kPostPostLoad (devbench's listener is up by then). Safe always.
	void Install();
}
