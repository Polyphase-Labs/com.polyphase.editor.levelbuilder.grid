# `com.polyphase.editor.levelbuilder.grid`

**Status:** Functional (basic) · **Layer:** sibling (depends on `core`) · **Hot-reload safe:** yes

Uniform-grid block placement for the Polyphase editor. Think
Roblox / Minecraft-style block builder, or generic prop-on-grid placement
for top-down or first-person dungeon kits. Sibling to
[`modular`](../com.polyphase.editor.levelbuilder.modular/README.md) —
they share the same kit registry through
[`core`](../com.polyphase.editor.levelbuilder.core/README.md)'s C-ABI.

---

## What it does

- **Grid Placement** tool — registers with core; activating it switches
  the active brush to *Grid Single* and the snap provider to *Grid Snap*.
- **`GridSnapProvider`** — uniform cell-size snap with configurable
  XYZ cell dimensions. No sockets; pure grid math.
- **`GridPlacementBrush`** — spawns `StaticMesh` or `Scene` assets the
  same way modular does (collision/overlap toggles not yet plumbed
  — see *Missing* below).
- **Grid tab in the Level Builder window:**
  - Kit dropdown (consumes core's `Kit_*` ABI — same kits show up here
    as in modular).
  - Cell-size sliders.
  - Grid overlay toggle.
  - Piece picker (text list today; thumbnail browser is the next port
    from modular).
- **Viewport overlay** — wire cubes at each cell within a camera-relative
  radius so you can see where placements will land.

## What's missing

Tracked in [`Packages.md`](../../Documentation/Developers/Packages.md) §4:

1. **Per-grid kit subset** — every kit shows up in the picker today,
   including socket-heavy modular pieces that don't really make sense
   on a grid. Add a `gridCompatible` JSON flag and filter on it.
2. **Configurable rotation snap** — currently 90° on Y only.
3. **Vertical-stack snap** — separate Y cell-size so multi-storey
   builds are easier.
4. **Per-piece collision / overlap toggles** — same UX as modular,
   ideally living in core so both siblings read/write the same state.
5. **Thumbnail piece picker** — grid still uses a text list; modular's
   thumbnail browser is the natural port target.

**Already done** (was on this list earlier):

- ✓ Spawn-fn registration for `tool.core` — grid registers
  `GridSpawnAtTransform` under tool name `"Grid Placement"`, so every
  brush in `tool.core` (Line / Box / BoxFill / NoiseFill / Paint /
  Replace / MaskFill) works in Grid mode too.
- ✓ Persistent placed-piece registry + rebuild-from-world on mode
  activate (matches modular's behavior — uses the v13 core ABI hook).

## Build

```bat
cd Packages\com.polyphase.editor.levelbuilder.grid
build.bat
```

Linux: `./build.sh`. Output:
`build/Windows/x64/<config>/com.polyphase.editor.levelbuilder.grid.dll`.

Must follow a core rebuild — the ABI strict-equality gate silently
short-circuits siblings built against an older header. Reload via
**Tools → Addons → Reload Native Addons**.

## Source layout

```
Source/
├── ComPolyphaseEditorLevelbuilderGrid.cpp     ← plugin entry
├── LevelBuilderCoreAPI.h                      ← synced copy from core
├── LevelBuilderInterfaces.h                   ← synced copy from core
├── LevelBuilderCoreLoader.{h,cpp}             ← late-bound resolver
├── GridPlacement.{h,cpp}                      ← tool, brush, snap provider, spawn-fn
├── GridUI.{h,cpp}                             ← Grid tab + overlay
└── ThumbnailCache.{h,cpp}                     ← PNG → ImTextureID
```

## Where to read next

- [`../../Documentation/Developers/Packages.md`](../../Documentation/Developers/Packages.md)
  §4 — grid's full status + handoff prompts (most notably the "tag grid-
  friendly pieces" prompt for the JSON schema extension).
- [`../com.polyphase.editor.levelbuilder.modular/Source/ModularPlacement.cpp`](../com.polyphase.editor.levelbuilder.modular/Source/ModularPlacement.cpp)
  `ModularSpawnAtTransform` — the canonical sibling spawn-fn signature.
  Grid needs the matching `GridSpawnAtTransform` to enable tool.core
  brushes.
- [`../com.polyphase.editor.levelbuilder.tool.core/README.md`](../com.polyphase.editor.levelbuilder.tool.core/README.md)
  — the brushes you'll be able to use once grid registers its spawn-fn.
