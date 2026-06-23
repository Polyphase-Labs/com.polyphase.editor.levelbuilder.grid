/**
 * @file GridPlacement.h
 * @brief Concrete LevelBuilderTool / Brush / SnapProvider implementations
 *        for grid-snap placement.
 *
 * Lifetime: instances are created in GridPlacement::Initialize and
 * destroyed in GridPlacement::Shutdown. Both are called from the addon
 * entry point.
 */

#pragma once

#include "LevelBuilderInterfaces.h"

#include <string>
#include <vector>

// Minimal placement bookkeeping — same shape as modular's
// ModularPlacedRegistry but without the kit/piece metadata since the
// grid brush doesn't need it for Erase (Paint's RMB-drag just wants the
// node pointer to destroy). Same place we'd grow this struct out for
// Replace / RebuildFromWorld in a later phase.
struct GridPlacedPiece
{
    void*       node = nullptr;
    std::string assetName;
    LBVec3      worldPos{0,0,0};
    LBQuat      worldRot{0,0,0,1};
};

class GridPlacedRegistry
{
public:
    static GridPlacedRegistry& Get();

    void Add(void* node, const std::string& assetName,
             const LBVec3& pos, const LBQuat& rot);
    void Clear();
    void RemoveByNode(void* node);

    // Walk the active world and re-register every StaticMesh3D whose
    // mesh matches a known kit-piece asset. Used to recover Paint
    // Erase / Replace state across project save+load. Returns count.
    int  RebuildFromWorld();

    int  Count() const { return (int)mPieces.size(); }
    const GridPlacedPiece& At(int i) const { return mPieces[i]; }

private:
    std::vector<GridPlacedPiece> mPieces;
};

class GridSnapProvider : public LevelBuilderSnapProvider
{
public:
    const char* GetName() const override { return "Grid (uniform)"; }

    bool GetSnapTransform(
        const LBVec3& rawPosition,
        LBVec3& outPosition,
        LBQuat& outRotation) override;
};

class GridPlacementBrush : public LevelBuilderBrush
{
public:
    const char* GetName() const override { return "Grid Single"; }
    const char* GetOwnerTool() const override { return "Grid Placement"; }

    bool CanPlace(const LevelBuilderPlacementRequest& request) override;
    LevelBuilderPlacementResult Place(const LevelBuilderPlacementRequest& request) override;

    // Drawn in the core Brush tab when this brush is active. Forwards to
    // GridUI::DrawSharedSections so the Brush tab shows the same kit /
    // piece / placement UI the extension tab does — and looks like the
    // Modular addon's tab.
    void DrawSettingsUI() override;
};

class GridLevelBuilderTool : public LevelBuilderTool
{
public:
    const char* GetName() const override { return "Grid Placement"; }

    void Activate(LevelBuilderContext* ctx) override;
    void Deactivate() override;
    void DrawSettingsUI() override;
};

namespace GridPlacement
{
    void Initialize();   // creates singletons + registers with core
    void Shutdown();     // unregisters and destroys singletons

    // Refresh the core palette named "Grid: <active kit name>" from
    // core's kit registry (via the v3 ABI). Called after Reload Kits.
    void RefreshPaletteForActiveKit();

    // Apply the current preview transform as an actual placement.
    void PlaceFromPreview();

    // Per-frame editor tick — wired through the plugin descriptor's
    // TickEditor slot. Handles the R-key rotate hotkey when Grid
    // Placement is the active tool. The rotation is written into core's
    // preview transform so both the wire-cube overlay AND tool.core's
    // Line/Box brushes pick it up per step.
    void TickEditor(float deltaTime);

    // Rotation hotkey state — file-static in GridPlacement.cpp's anon
    // namespace, exposed here so future UI knobs (yaw slider in the
    // Brush tab, mirror of modular's Rotation section) can read/write
    // the same source of truth.
    float GetYawDeg();
    void  SetYawDeg(float deg);
    void  AddYawDelta(float deg);

    // ---- snap settings -------------------------------------------------

    // Cell size in world units. The snap provider rounds rawPosition to
    // the nearest cell center along each axis (so X=Y=Z=1 → unit-cube
    // grid; X=Z=4, Y=1 → 4 m tile grid with 1 m vertical steps).
    void   SetCellSize(float x, float y, float z);
    void   GetCellSize(float* outX, float* outY, float* outZ);

    // Yaw quantization in degrees (default 90°). Rotation snaps round to
    // the nearest multiple. Other axes pass through unchanged.
    void   SetYawStepDeg(float deg);
    float  GetYawStepDeg();

    // Per-axis grid origin offset. Useful when the artist's pivot doesn't
    // sit at a cell center.
    void   SetGridOrigin(float x, float y, float z);
    void   GetGridOrigin(float* outX, float* outY, float* outZ);

    // ---- viewport overlay -----------------------------------------------

    // Toggle the wire-cube grid overlay that follows the preview position.
    void SetOverlayEnabled(bool enabled);
    bool GetOverlayEnabled();

    // Number of cells to draw on each side of the center; total cells
    // drawn = (2N+1)^2 in the XZ plane.
    void SetOverlayRadius(int n);
    int  GetOverlayRadius();
}

// Viewport overlay trampoline — defined in GridPlacement.cpp, declared
// here so the entry point can pass it to EditorUIHooks::RegisterViewportOverlay.
extern "C" void GridPlacement_DrawViewportOverlayTrampoline(
    float x, float y, float w, float h, void* userData);
