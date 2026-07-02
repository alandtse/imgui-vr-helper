# VRIK mod-support interface (reference copy)

`vrikinterface001.h` / `vrikinterface001.cpp` are VRIK's own public mod-support SDK
(IVrikInterface001, revision 1; VRIK build 80400), reproduced here **unmodified** for
provenance — VRIK's full source isn't public, but the author distributes this small
interop header for other mod developers to build compatibility against, matching how
similar Skyrim VR mods (e.g. HIGGS) publish support headers separately from their
closed main codebase. No license file or copyright header ships with the originals.

These files are **not compiled** as part of this project (they use the older skse64
`PluginAPI.h`/`NiTypes.h` headers, not CommonLibVR) and are excluded from the source
glob by living outside `src/`. The actual interop code this project builds against is
`src/VrikCompat.cpp`, which reproduces only the ABI-relevant parts of
`IVrikInterface001` adapted to this project's own types (`SKSE::MessagingInterface`,
`RE::NiPoint3`) — keep the two in sync if VRIK ever ships a new interface revision.
