# Handoff: wand-marker / panel-content pointer divergence

Status as of 2026-07-04. Written for a fresh agent picking this up cold — includes raw data,
not just conclusions, so you can check the reasoning or draw a different one.

**Note:** this branch has had concurrent work land after this doc's raw data was captured
(commits `bf7df8e`, `3de6cd7`, `15573ab` — a separate session, different from the one that wrote
this doc). Notably: `kFrameFlag_SuppressInputForwarding` has been replaced by a new
"route-on-press input leases" mechanism (`src/internal/InputLeases.h`) as the default path (the
old latch logic this doc's item 4 below refers to is kept only as a `useInputLeases=false`
fallback); the cursor marker gained a seam fix; and the `TriggerPress NDC` diagnostic was demoted
from `logs::info` to `logs::debug` (so you'll need to bump the configured log level to see it
now). None of this changes the pointer-divergence findings below — the marker/UV math this doc
is about wasn't touched by those commits — but check current source before assuming anything
about the input-forwarding path specifically, since that subsystem was substantially rewritten
out from under this doc.

## Symptom (as reported by the user, verbatim characterization)

Testing in Skyrim VR across several client mods that all use this helper (PhotoMode,
SKSE-Menu-Framework-3, open-shaders, DialogueHistory, and the helper's own settings UI):

> "the divergence is totally dependent on the client mod. Open Shaders has no mouse divergence.
> Photo Mode has the largest. Dialogue history and the helper menu itself have about medium
> pointer divergence."

i.e. the helper draws a cursor marker (a small dot, `RenderCursorPass` in
`src/InSceneOverlay.cpp`) at the wand's computed hit position on the focused client's panel. For
some clients the dot visually tracks correctly; for others it visibly drifts from where the
client's own content/buttons actually are, worse toward panel edges, and the amount of drift is
client-dependent (not a fixed pixel offset).

Separately (same investigation, related symptom): clicking at a spot that visually looks right
sometimes doesn't register on the intended widget.

## What's already been fixed in this branch (`feat/panel-cursor-marker`, PR #19) — ruled out

These were real bugs found and fixed along the way, but **none of them turned out to be the
main divergence** (confirmed by the fact that the divergence is still reported after all of
these landed):

1. **DisplaySize aspect mismatch** — DialogueHistory was using the raw desktop screen size for
   `io.DisplaySize` instead of the VR panel's actual aspect (`VR::GetPanelSize`). Fixed
   client-side (DialogueHistory, not this repo). Ruled out as the sole cause because the
   divergence is also seen on the helper's own settings client, which doesn't have this bug.
2. **`m_lastOpenedOverlay` poisoning** by PointerFocus/WorldQuad clients — a focus-routing bug,
   unrelated to pointer coordinates.
3. **Diagnostic desync** — an earlier version of the marker-vs-panel diagnostic compared
   independently-throttled counters in two different render passes, so it was comparing samples
   from different frames under head/panel motion and manufacturing fake "divergence." Fixed by
   computing both marker and panel edge NDC in the _same_ function call from the _same_
   anchor/vpMat (see `RenderCursorPass`, "Sync NDC" → later "TriggerPress NDC").
4. **`kFrameFlag_SuppressInputForwarding` stuck during client-requested drags**, and
   **`io.MousePos` teleporting to `(-FLT_MAX,-FLT_MAX)` while a widget is active** — both real
   bugs (see commits `3879b58`, `6f716b5`), but about _stuck interaction state_, not about wand
   UV being wrong. Both fixed, PR #19.

## What's confirmed NOT the cause

**The marker's computed position is internally self-consistent.** `ResolveAnchor()` (extracted in
commit `32113fe`) is the single shared function `RenderPanelPass` and `RenderCursorPass` both use
to resolve the panel's anchor transform and view-projection matrix. The marker's model matrix is
built from the exact same `wand.uvCoordinates` the panel-hit-test produced, composed through the
same `Config::CreateScaleMatrix(s.menuScale) * anchor` chain the panel quad itself uses. A
diagnostic that computes the panel's own left/center/right edge NDC and the marker's NDC in the
same call, from the same instant, shows the marker interpolates correctly between the panel edges
(see raw data below) — this is expected, since it's the same formula, but it does rule out a
transform-math bug in the marker draw itself.

**The wand-hit-test UV is consistent between the helper and the client.** This is the important
new result from this round. Two diagnostics were added, both gated on the _actual trigger-press
edge_ (not a periodic timer, to get ground-truth instants rather than arbitrary samples):

- Helper side (`src/InSceneOverlay.cpp`, `RenderCursorPass`): logs `TriggerPress NDC: uv=(...)
local=(...) marker=(...) panelLeft=(...) panelCenter=(...) panelRight=(...)` on the rising edge
  of either controller's trigger.
- Client side (DialogueHistory, `src/ImGui/Renderer.cpp`, not in this repo): logs `Client click:
mousePos=(...) displaySize=(...) uv=(...) hoveredId=... activeId=...` on
  `ImGui::IsMouseClicked(ImGuiMouseButton_Left)`.

Matching entries by UV value across both logs (raw data in `raw-helper-log.txt` /
`raw-dialoguehistory-log.txt`, correlated table below) shows the two sides agree on the wand's UV
to within ~0.01–0.02 for the same physical click, consistently. **This means the compositor's
raycast (used to draw the marker) and the client's own `io.MousePos` (used for
hover/click/widget resolution) are receiving the same value.** A bug in that shared computation
would show up as a UV _disagreement_ between the two logs; it doesn't.

## What's NOT yet explained

Despite the UV pipeline being consistent, a meaningful fraction of clicks land on nothing
(`hoveredId=0` — no widget under the cursor at all). In the most recent test session (raw data
below), of 7 correlated clicks, 2 hit nothing and 5 hit the same widget consistently. In an
earlier session, of 10 correlated clicks on the Global (archive) panel, 6 hit nothing and 4 hit
something.

Since the UV computation is provably shared and consistent, a miss isn't a UV-computation bug —
if it were, the two logs would disagree with each other, and they don't. Two hypotheses remain,
**neither has been tested yet**:

1. **Marker rendering doesn't visually land where its computed NDC says it should.** All
   verification so far has been "what NDC did the code intend to draw the marker at" — nothing
   has verified "where did the marker quad actually rasterize on screen, and does that match
   where the panel's own content pixels are for that same UV." This needs a live GPU frame
   capture (RenderDoc — available as an MCP tool in this session/environment, `mcp__renderdoc__*`)
   comparing the marker draw call's actual output position against the panel draw call's, in the
   same captured frame. This is the highest-value next step and hasn't been attempted yet.
2. **Aiming precision, not a bug.** `kMarkerLocalSize = 0.016` (fraction of panel local space) is
   small. If the user is aiming based on the drawn dot and the dot is genuinely hard to place
   precisely at typical wand-tracking jitter, misses could be a UX/scale problem rather than a
   coordinate bug. This wouldn't explain the reported _client-dependent_ severity, though, unless
   different clients have differently-sized/spaced clickable widgets (a tight cluster of small
   buttons vs. sparser large ones) — which would make the same absolute aiming error produce a
   different apparent "divergence" per client without any positioning bug at all. **Not
   distinguished from hypothesis 1 yet** — a RenderDoc capture would distinguish them: if the
   marker's rasterized position matches its computed NDC, this points toward (2); if it doesn't,
   toward (1).

Neither hypothesis explains the _client-dependent_ part on its own without added assumptions —
that's the main open question. Possible angle not yet explored: repeat the exact same
trigger-press-gated diagnostic on a client with reportedly _zero_ divergence (open-shaders) and
one with the _worst_ divergence (PhotoMode), and compare hit rates and UV-agreement the same way
this doc does for DialogueHistory. If open-shaders shows ~100% hit rate with the same UV-agreement
precision, and PhotoMode shows markedly worse, that localizes the remaining variable to something
client-side (its own widget layout/density, or its own `io.DisplaySize`/panel-size setup) rather
than the helper's shared marker/UV code — since the shared code produces the same result
regardless of which client is focused.

## Raw data

Full log files as captured, copied verbatim into this directory:

- `raw-helper-log.txt` — full `ImGuiVRHelper.log` for the session, includes all `TriggerPress NDC`
  lines plus other operational logging (connect/focus/etc.) for context.
- `raw-dialoguehistory-log.txt` — full `po3_DialogueHistory.log` for the same session, includes
  all `Client click` lines plus `VR state` transition logs for context.

### Correlated table (most recent session, by UV match)

Matched by nearest UV value between the two logs (no shared timestamp — DialogueHistory's logger
doesn't prefix timestamps, the helper's does `[HH:MM:SS.mmm]`). Session covers the DialogueHistory
Local (conversation) panel.

| Helper `uv`   | Helper `marker` NDC | Helper panel L/C/R NDC.x | Client `uv`   | Client `hoveredId` |
| ------------- | ------------------- | ------------------------ | ------------- | ------------------ |
| (0.609,0.434) | (0.138,0.097)       | -0.833 / 0.024 / 0.429   | (0.609,0.434) | 0 (miss)           |
| (0.117,0.127) | (0.044,0.213)       | -0.112 / 0.496 / 0.989   | (0.116,0.127) | 704808852 (hit)    |
| (0.123,0.133) | (0.027,0.214)       | -0.138 / 0.475 / 0.965   | (0.123,0.133) | 704808852 (hit)    |
| (0.124,0.123) | (0.015,0.229)       | -0.143 / 0.462 / 0.936   | (0.124,0.123) | 704808852 (hit)    |
| (0.116,0.125) | (0.039,0.254)       | -0.119 / 0.518 / 1.020   | (0.115,0.124) | 704808852 (hit)    |
| (0.117,0.113) | (0.011,0.266)       | -0.151 / 0.488 / 0.976   | (0.118,0.117) | 704808852 (hit)    |
| (0.943,0.902) | (0.360,-0.045)      | -0.975 / -0.005 / 0.401  | (0.943,0.901) | 0 (miss)           |

Note the panel's own center NDC.x wanders across entries (0.024, 0.496, 0.475, 0.462, 0.518,
0.488, -0.005) even though several of these clicks are at nearly the same UV — this reflects the
panel moving in view (head/panel motion between clicks, expected) or possibly `AttachMode` /
`positioningMethod` changes; not yet checked which. Worth confirming this movement is legitimate
(head tracking) and not itself a symptom, before ruling it out as a contributing factor to
per-click error.

### Earlier session (Global/archive panel), for reference

10 correlated clicks, 4 hits / 6 misses. See prior conversation history / git blame on
`src/InSceneOverlay.cpp` around the "Sync NDC" → "TriggerPress NDC" diagnostic evolution for the
original log lines (not re-included here verbatim; the pattern — UV agreement, majority miss
rate — was the same shape as the table above).

## Instrumentation currently in the tree (still TEMP, not yet removed)

- `src/InSceneOverlay.cpp`, `RenderCursorPass`: `TriggerPress NDC` log, edge-gated on either
  controller's trigger via `RE::BSOpenVRControllerDevice::Keys::kTrigger`. Flagged in PR #19
  review (Copilot) as a potential log-spam concern. As of commit `3de6cd7` this is now
  `logs::debug` (was `logs::info` when the raw data below was captured) — bump the configured
  log level to see it in a fresh capture. Still needed for this investigation; remove once
  root-caused.
- DialogueHistory (sibling repo, not here): `src/ImGui/Renderer.cpp`, `Client click` log in
  `RenderMenuFrame`, gated on `ImGui::IsMouseClicked(ImGuiMouseButton_Left)`. Same
  remove-before-ship caveat.

## Suggested next steps, roughly in priority order

1. **RenderDoc capture** during a reproducible divergence (DialogueHistory or PhotoMode, whichever
   is easier to get into a stable "wand pointing at a specific known button" state) — compare the
   marker draw call's actual rasterized screen position against the panel draw call's, for the
   same eye/frame. This directly tests hypothesis 1 above.
2. **Repeat the trigger-press-gated dual-log diagnostic on open-shaders and PhotoMode** (both
   already use this helper) to get a hit-rate and UV-agreement baseline for the "no divergence"
   and "worst divergence" ends of the reported spectrum, using the exact same method as this doc.
   If UV agreement holds everywhere (likely, since it's shared code) but hit-rate varies sharply,
   that points at something in each client's own panel layout/density rather than the helper.
3. Consider a controlled test: have a client draw one large, fixed-position, known-UV button
   (e.g. dead center, uv=(0.5,0.5)) and log hit/miss + marker NDC for repeated deliberate clicks
   at it, removing "did the user aim well" as a variable.
