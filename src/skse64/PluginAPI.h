#pragma once
// Compat shim so third_party/vrikinterface/vrikinterface001.h (VRIK's unmodified public SDK
// header) can be #included as-is against this project's CommonLibVR types, instead of the
// legacy skse64 headers it was written against (which this project doesn't otherwise use).
//
// Only PluginHandle needs a real definition — it's the type actually referenced by name in
// IVrikInterface001's sibling free-function declaration (getVrikInterface001), which this
// project doesn't call (see src/VrikCompat.cpp, which implements its own loader against
// SKSE::MessagingInterface's modern 4-arg Dispatch instead of this legacy 5-arg one).
// SKSEMessagingInterface only needs to be a valid type name for that declaration to parse.

#include "SKSE/Impl/Stubs.h"

using PluginHandle = SKSE::PluginHandle;
class SKSEMessagingInterface;
