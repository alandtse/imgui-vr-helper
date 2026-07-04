// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2026 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// The devbench client API ships its SKSE-messaging handshake stub as source
// (it must compile inside the consumer to share its CommonLib). The package
// installs it under include/; this TU builds it.

#include "pch.h"

#include <DevBenchAPI.cpp>
