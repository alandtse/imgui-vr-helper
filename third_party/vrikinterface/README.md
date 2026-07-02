# VRIK mod-support interface (reference copy)

`vrikinterface001.h` / `vrikinterface001.cpp` are VRIK's own public mod-support SDK
(IVrikInterface001, revision 1), reproduced here **unmodified** for provenance.
VRIK: https://www.nexusmods.com/skyrimspecialedition/mods/23416 — its full source isn't
public, but the author distributes this small interop header specifically so other mod
developers can build compatibility against VRIK, the same purpose this vendoring serves
(matching how similar Skyrim VR mods, e.g. HIGGS, publish support headers separately from
their closed main codebase). No license file or copyright header ships with the
originals; if VRIK's author objects to this redistribution, remove on request.

This project's own license (COPYING) is GPL-3.0-or-later; EXCEPTIONS.md's "Modding
Exception" is written to cover exactly this situation — linking/combining this Program
with third-party "Modding Libraries" under unclear or GPL-incompatible licenses, which a
small interop-only ABI header meant for cross-mod compatibility falls squarely under.

These files are **not compiled** as part of this project (they use the older skse64
`PluginAPI.h`/`NiTypes.h` headers, not CommonLibVR) and are excluded from the source
glob by living outside `src/`. The actual interop code this project builds against is
`src/VrikCompat.cpp`, which `#include`s `vrikinterface001.h` directly through a small
`src/skse64/` compat shim (rather than hand-copying the interface declaration) — keep
watching for a new VRIK interface revision if this ever needs updating.
