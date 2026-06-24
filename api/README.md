# ImGuiVRHelper client API

LGPL-3.0-or-later. These are the only files a client mod consumes — the public
headers plus the handshake stub (`ImGuiVRHelperAPI.cpp`) — so a non-GPL mod can
vendor or link them without becoming GPL. The runtime helper (`src/`) is GPL-3.0
with a Skyrim modding exception; see the repository root `COPYING` and
`EXCEPTIONS.md`.

LGPL-3.0 is GPL-3.0 plus additional permissions, so this directory carries both
texts — [`COPYING`](./COPYING) (the GPL-3.0 it incorporates) and
[`COPYING.LESSER`](./COPYING.LESSER) (the additional permissions) — so a vendored
copy is self-sufficient.

Consume via CMake FetchContent (`SOURCE_SUBDIR api`, link `ImGuiVRHelper::api`) or
vendor these files directly. See the repository [README](../README.md) for
integration.
