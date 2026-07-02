#pragma once
// Compat shim so third_party/vrikinterface/vrikinterface001.h (VRIK's unmodified public SDK
// header) can be #included as-is; see PluginAPI.h in this directory for the full rationale.
// NiPoint3 (three floats, no vtable — RE::NiPoint3 static_asserts sizeof == 0xC) has an
// identical layout to VRIK's own skse64 NiPoint3, so exchanging it by value across the DLL
// boundary is ABI-compatible regardless of which header declares the name.

#include "RE/N/NiPoint3.h"

using NiPoint3 = RE::NiPoint3;
