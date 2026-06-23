#include "GridPlacement.h"

#include "LevelBuilderCoreLoader.h"

#if EDITOR
#include "GridUI.h"          // brush DrawSettingsUI forwards into GridUI::DrawSharedSections
#include "ThumbnailCache.h"  // released in Shutdown so descriptors don't leak across reload
#endif

#include "Plugins/PolyphaseEngineAPI.h"

// Direct engine class access (linked from Polyphase.lib) for the Scene
// vs StaticMesh spawn fork — mirrors modular's placement path.
#include "Engine/Nodes/3D/StaticMesh3d.h"
#include "Engine/Assets/StaticMesh.h"
#include "Engine/Assets/Scene.h"
#include "Engine/World.h"

#include "glm/vec3.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>

#if EDITOR
#include "imgui.h"
#endif

// -----------------------------------------------------------------------------
// Singletons + tunables
// -----------------------------------------------------------------------------

// Forward decl — defined further down; referenced by Initialize.
static void GridPlacement_OnViewportClick(const LBVec3* hitPos,
                                          const LBVec3* hitNormal,
                                          void*         hitNode,
                                          int           button,
                                          void*         userData);

namespace
{
    GridSnapProvider*     sSnap  = nullptr;
    GridPlacementBrush*   sBrush = nullptr;
    GridLevelBuilderTool* sTool  = nullptr;

    LevelBuilderCoreAPI* CoreAPI() { return LevelBuilderCoreLoader::Get(); }

    PolyphaseEngineAPI* EngineAPI()
    {
        LevelBuilderCoreAPI* c = CoreAPI();
        return c ? (PolyphaseEngineAPI*)c->GetEngineAPI() : nullptr;
    }

    // ---- snap settings ----
    float sCellX = 1.0f, sCellY = 1.0f, sCellZ = 1.0f;
    float sOriginX = 0.0f, sOriginY = 0.0f, sOriginZ = 0.0f;
    float sYawStepDeg = 90.0f;

    // ---- rotation hotkey state ----
    // R increments this by -90° while Grid Placement is the active tool.
    // The snap provider then quantizes to sYawStepDeg each frame, so the
    // value the user sees is always a multiple of the snap step.
    float sGridYawDeg = 0.0f;

    float WrapYaw(float a)
    {
        while (a >  180.0f) a -= 360.0f;
        while (a < -180.0f) a += 360.0f;
        return a;
    }

    // Write sGridYawDeg as a Y-rot quaternion into core's preview
    // transform, preserving the existing position and scale. Called from
    // TickEditor whenever the user nudges the yaw via R.
    void ApplyGridYawToPreview()
    {
        LevelBuilderCoreAPI* api = CoreAPI();
        if (!api || !api->GetPreviewTransform || !api->SetPreviewTransform) return;

        LBVec3 pos, scl; LBQuat rot;
        api->GetPreviewTransform(&pos, &rot, &scl);

        const float DEG2RAD = 3.14159265358979323846f / 180.0f;
        const float half = sGridYawDeg * DEG2RAD * 0.5f;
        rot.x = 0.0f;
        rot.y = std::sin(half);
        rot.z = 0.0f;
        rot.w = std::cos(half);
        if (scl.x == 0.0f && scl.y == 0.0f && scl.z == 0.0f)
            scl = LBVec3{1.0f, 1.0f, 1.0f};

        api->SetPreviewTransform(&pos, &rot, &scl);
    }

    // ---- overlay settings ----
    bool sOverlayOn      = true;
    int  sOverlayRadius  = 5;   // half-width in cells; total = (2N+1)^2

    float SnapAxis(float raw, float origin, float cell)
    {
        if (cell <= 0.000001f) return raw;
        float rel = raw - origin;
        float idx = std::floor(rel / cell + 0.5f);
        return origin + idx * cell;
    }

    float WrapAngleDeg(float a)
    {
        // [-180, 180)
        while (a >  180.0f) a -= 360.0f;
        while (a < -180.0f) a += 360.0f;
        return a;
    }

    // Quaternion helpers — pure yaw around Y for grid rotation.
    LBQuat YawDegToQuat(float yawDeg)
    {
        const float DEG2RAD = 3.14159265358979323846f / 180.0f;
        float half = yawDeg * DEG2RAD * 0.5f;
        LBQuat q;
        q.x = 0.0f;
        q.y = std::sin(half);
        q.z = 0.0f;
        q.w = std::cos(half);
        return q;
    }

    float QuatToYawDeg(const LBQuat& q)
    {
        // Extract yaw assuming the quat is (approximately) a Y-axis rotation.
        // This is the angle of (q.y, q.w) doubled.
        const float RAD2DEG = 180.0f / 3.14159265358979323846f;
        return 2.0f * std::atan2(q.y, q.w) * RAD2DEG;
    }

    // ----- Undo userData shared by spawn / destroy actions (v9) -----
    //
    // Same shape as modular's ModularUndoRecord; symmetric EnsureAlive /
    // EnsureDead helpers feed both Spawn-action and Destroy-action
    // wiring. See ModularPlacement.cpp for the full design notes.
    struct GridUndoRecord
    {
        std::string assetName;
        LBVec3      pos{0,0,0};
        LBQuat      rot{0,0,0,1};
        void*       node = nullptr;
    };

    static void* GridSpawnAtTransform(const char* assetName,
                                      const LBVec3* pos,
                                      const LBQuat* rot,
                                      void*         userData);

    static void Grid_EnsureAlive(void* p);
    static void Grid_EnsureDead(void* p);
    static void Grid_FreeRecord(void* p) { delete (GridUndoRecord*)p; }

    thread_local bool sReplayingAction = false;

    // ----- Spawn-fn callback used by tool.core's generic brushes -----
    //
    // tool.core's brushes (Line, Box, BoxFill, …) don't know how to spawn
    // grid pieces — they call api->GetSpawnFnForActiveTool() to find the
    // active sibling's spawn fn, then invoke it once per shape point. We
    // route every spawn through GridPlacementBrush::Place so the StaticMesh-
    // vs-Scene auto-detect + transform application live in one place.
    // Always spawns at native scale (1,1,1) — the Place path's optional
    // request.scale field is preserved verbatim for callers that supply it.
    //
    // Routes via sBrush->Place() directly (bypassing api->Place which
    // dispatches to the active brush). That matters when the active brush
    // is tool.core's Line/Box — otherwise we'd recurse forever.
    static void* GridSpawnAtTransform(const char*   assetName,
                                      const LBVec3* pos,
                                      const LBQuat* rot,
                                      void*         /*userData*/)
    {
        LevelBuilderCoreAPI* api = CoreAPI();
        if (!api) return nullptr;

        const char* effectiveAsset = assetName;
        if (!effectiveAsset || !*effectiveAsset)
        {
            // Fall back to the active palette piece.
            LevelBuilderPalette* pal = api->GetActivePalette
                                       ? api->GetActivePalette() : nullptr;
            if (!pal) return nullptr;
            int idx = api->Palette_GetActiveIndex(pal);
            if (idx < 0) return nullptr;
            LevelBuilderPaletteItem it{};
            if (!api->Palette_GetItem(pal, idx, &it)) return nullptr;
            effectiveAsset = it.assetName;
        }
        if (!effectiveAsset) return nullptr;

        LevelBuilderPlacementRequest req{};
        req.assetName  = effectiveAsset;
        req.position   = pos ? *pos : LBVec3{0, 0, 0};
        req.rotation   = rot ? *rot : LBQuat{0, 0, 0, 1};
        req.scale      = LBVec3{1.0f, 1.0f, 1.0f};
        req.parentNode = nullptr;

        LevelBuilderPlacementResult r{};
        if (sBrush) r = sBrush->Place(req);
        else if (api->Place) r = api->Place(&req);
        return r.success ? r.spawnedNode : nullptr;
    }

    // ----- v7: placed-piece enumeration for Paint Erase / Replace -----
    //
    // Same shape as modular's enumerate fn — visit() returning 1 means
    // "consume" so we destroy the node + drop it from the placed
    // registry in one transaction, preventing the use-after-free that
    // would otherwise happen with Paint's 60Hz tick re-feeding
    // dangling pointers.
    static void GridEnumeratePlacements(const LBVec3* center, float radius,
                                         const char* sourceAssetFilter,
                                         LevelBuilderCoreAPI::LBVisitFn visit,
                                         void* visitUd,
                                         void* /*siblingUd*/)
    {
        if (!center || !visit || radius <= 0.0f) return;
        const float r2 = radius * radius;
        auto& reg = GridPlacedRegistry::Get();

        // v11: optional source-asset filter.
        const bool haveFilter = (sourceAssetFilter && *sourceAssetFilter);

        // v12: also snapshot the asset alongside the node so the visitor
        // can read it (Replace's eyedropper mode).
        struct Hit { void* node; std::string asset; };
        std::vector<Hit> hits;
        hits.reserve(16);
        for (int i = 0; i < reg.Count(); ++i)
        {
            const auto& p = reg.At(i);
            const float dx = p.worldPos.x - center->x;
            const float dz = p.worldPos.z - center->z;
            if (dx*dx + dz*dz > r2) continue;
            if (haveFilter && p.assetName != sourceAssetFilter) continue;
            hits.push_back({p.node, p.assetName});
        }

        PolyphaseEngineAPI* eng = nullptr;
        for (const Hit& h : hits)
        {
            void* node = h.node;
            if (!node) continue;
            int consume = visit(node, h.asset.c_str(), visitUd);
            if (consume != 1) continue;

            // Snapshot asset+pos+rot BEFORE registry removal — undo
            // needs them to re-spawn on Ctrl+Z.
            std::string asset;
            LBVec3 pos{0,0,0}; LBQuat rot{0,0,0,1};
            for (int i = 0; i < reg.Count(); ++i)
            {
                if (reg.At(i).node == node)
                {
                    asset = reg.At(i).assetName;
                    pos   = reg.At(i).worldPos;
                    rot   = reg.At(i).worldRot;
                    break;
                }
            }

            if (!eng) eng = EngineAPI();
            if (eng && eng->DestroyNode) eng->DestroyNode((Node*)node);
            reg.RemoveByNode(node);

            // v9: push a Destroy action. Initial state already dead, so
            // do=EnsureDead is a no-op first call. Undo=EnsureAlive
            // re-spawns; Redo re-runs EnsureDead.
            if (eng && eng->EditorAction_Push && !sReplayingAction)
            {
                auto* rec = new GridUndoRecord();
                rec->assetName = asset;
                rec->pos       = pos;
                rec->rot       = rot;
                rec->node      = nullptr;
                eng->EditorAction_Push("level-builder.grid.destroy",
                                       /*do=*/   &Grid_EnsureDead,
                                       /*undo=*/ &Grid_EnsureAlive,
                                       /*free=*/ &Grid_FreeRecord,
                                       rec);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// GridPlacedRegistry
// -----------------------------------------------------------------------------

GridPlacedRegistry& GridPlacedRegistry::Get()
{
    static GridPlacedRegistry sInstance;
    return sInstance;
}

void GridPlacedRegistry::Add(void* node, const std::string& assetName,
                              const LBVec3& pos, const LBQuat& rot)
{
    if (!node) return;
    GridPlacedPiece p;
    p.node      = node;
    p.assetName = assetName;
    p.worldPos  = pos;
    p.worldRot  = rot;
    mPieces.push_back(std::move(p));
}

void GridPlacedRegistry::Clear()
{
    mPieces.clear();
}

void GridPlacedRegistry::RemoveByNode(void* node)
{
    if (!node) return;
    for (auto it = mPieces.begin(); it != mPieces.end(); ++it)
    {
        if (it->node == node) { mPieces.erase(it); return; }
    }
}

int GridPlacedRegistry::RebuildFromWorld()
{
    // Same pattern as modular's RebuildFromWorld — walk the scene tree,
    // match StaticMesh3D nodes against the known asset-name set, register
    // matches. Grid doesn't carry kit metadata on each piece, so we just
    // need the asset-name set rather than a kit-name → piece-name map.
    Clear();

    LevelBuilderCoreAPI* api = CoreAPI();
    if (!api) return 0;
    PolyphaseEngineAPI* eng = EngineAPI();
    if (!eng || !eng->GetWorld) return 0;

    World* world = (World*)eng->GetWorld(0);
    if (!world) return 0;
    Node* root = world->GetRootNode();
    if (!root) return 0;

    // Set of asset names across all loaded kits.
    std::unordered_set<std::string> knownAssets;
    const int kitCount = api->Kit_GetCount ? api->Kit_GetCount() : 0;
    for (int ki = 0; ki < kitCount; ++ki)
    {
        LBKitInfo k{};
        if (!api->Kit_GetInfo || !api->Kit_GetInfo(ki, &k)) continue;
        for (int pi = 0; pi < k.pieceCount; ++pi)
        {
            LBPieceInfo p{};
            if (!api->Kit_GetPieceInfo || !api->Kit_GetPieceInfo(ki, pi, &p)) continue;
            if (p.assetName && *p.assetName) knownAssets.insert(p.assetName);
        }
    }
    if (knownAssets.empty()) return 0;

    std::vector<Node*> stack;
    stack.reserve(64);
    stack.push_back(root);
    int found = 0;
    while (!stack.empty())
    {
        Node* n = stack.back();
        stack.pop_back();
        if (!n) continue;

        const uint32_t kids = n->GetNumChildren();
        for (uint32_t i = 0; i < kids; ++i)
        {
            if (Node* c = n->GetChild((int32_t)i))
                stack.push_back(c);
        }

        if (!n->Is("StaticMesh3D")) continue;
        StaticMesh3D* sm = (StaticMesh3D*)n;
        StaticMesh* mesh = sm->GetStaticMesh();
        if (!mesh) continue;
        const std::string& assetName = mesh->GetName();
        if (knownAssets.find(assetName) == knownAssets.end()) continue;

        glm::vec3 wp = sm->GetWorldPosition();
        glm::quat wr = sm->GetWorldRotationQuat();
        Add((void*)n, assetName,
            LBVec3{wp.x, wp.y, wp.z},
            LBQuat{wr.x, wr.y, wr.z, wr.w});
        ++found;
    }

    if (eng->LogDebug)
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "[LevelBuilderGrid] RebuildFromWorld: re-registered %d placed piece(s)",
                      found);
        eng->LogDebug(buf);
    }
    return found;
}

// -----------------------------------------------------------------------------
// GridSnapProvider
// -----------------------------------------------------------------------------

bool GridSnapProvider::GetSnapTransform(
    const LBVec3& rawPosition,
    LBVec3& outPosition,
    LBQuat& outRotation)
{
    outPosition.x = SnapAxis(rawPosition.x, sOriginX, sCellX);
    outPosition.y = SnapAxis(rawPosition.y, sOriginY, sCellY);
    outPosition.z = SnapAxis(rawPosition.z, sOriginZ, sCellZ);

    // Read whatever rotation the preview already has, snap its yaw to the
    // configured step. Pitch/roll are ignored — grid pieces are upright.
    LevelBuilderCoreAPI* api = CoreAPI();
    if (api && api->GetPreviewTransform)
    {
        LBVec3 _p, _s; LBQuat curRot;
        api->GetPreviewTransform(&_p, &curRot, &_s);
        float yawDeg = WrapAngleDeg(QuatToYawDeg(curRot));
        if (sYawStepDeg > 0.001f)
            yawDeg = std::round(yawDeg / sYawStepDeg) * sYawStepDeg;
        outRotation = YawDegToQuat(yawDeg);
    }
    else
    {
        outRotation = LBQuat{0, 0, 0, 1};
    }
    return true;
}

// -----------------------------------------------------------------------------
// GridPlacementBrush
// -----------------------------------------------------------------------------

bool GridPlacementBrush::CanPlace(const LevelBuilderPlacementRequest& request)
{
    return request.assetName && *request.assetName;
}

void GridPlacementBrush::DrawSettingsUI()
{
    // No brush-specific knobs. The full kit/piece/cell-size UI is drawn
    // by GridLevelBuilderTool::DrawSettingsUI in the Brush tab — it
    // persists across brush changes (including tool-agnostic ones).
}

LevelBuilderPlacementResult GridPlacementBrush::Place(const LevelBuilderPlacementRequest& request)
{
    LevelBuilderPlacementResult result{};
    result.success      = 0;
    result.spawnedNode  = nullptr;
    result.errorMessage = nullptr;

    PolyphaseEngineAPI* api = EngineAPI();
    if (!api || !api->GetWorld || !api->SpawnNode || !api->Node3D_SetPosition)
    {
        static const char* kErr = "Engine API not ready";
        result.errorMessage = kErr;
        return result;
    }

    World* world = (World*)api->GetWorld(0);
    if (!world)
    {
        static const char* kErr = "No active world";
        result.errorMessage = kErr;
        return result;
    }

    // Same Scene/StaticMesh fork as modular's brush — Scene assets go
    // through World::SpawnScene (gets the full prefab hierarchy), other
    // asset types fall back to StaticMesh3D + SetStaticMesh.
    Asset* asset = api->LoadAsset ? api->LoadAsset(request.assetName) : nullptr;
    Node*  node  = nullptr;

    if (asset && asset->Is("Scene"))
    {
        glm::vec3 spawnPos(request.position.x, request.position.y, request.position.z);
        node = world->SpawnScene((Scene*)asset, spawnPos);
    }
    else
    {
        node = api->SpawnNode(world, "StaticMesh3D");
        if (node && asset && asset->Is("StaticMesh"))
        {
            ((StaticMesh3D*)node)->SetStaticMesh((StaticMesh*)asset);
        }
        else if (!asset && api->LogWarning)
        {
            api->LogWarning("[LevelBuilderGrid] asset '%s' not found — placed empty StaticMesh3D",
                            request.assetName);
        }
    }

    if (!node)
    {
        static const char* kErr = "Failed to spawn node (Scene/StaticMesh3D unregistered?)";
        result.errorMessage = kErr;
        return result;
    }

    if (node->IsNode3D() && api->Node3D_SetPosition)
        api->Node3D_SetPosition((Node3D*)node, request.position.x, request.position.y, request.position.z);

    // Yaw-only rotation for grid — extract the yaw from the requested quat
    // and feed it through SetRotation. Pitch/roll deliberately zero.
    if (node->IsNode3D() && api->Node3D_SetRotation)
    {
        float yawDeg = QuatToYawDeg(request.rotation);
        api->Node3D_SetRotation((Node3D*)node, 0.0f, yawDeg, 0.0f);
    }

    if (node->IsNode3D() && api->Node3D_SetScale)
        api->Node3D_SetScale((Node3D*)node, request.scale.x, request.scale.y, request.scale.z);

    // v7: track the placement so the Paint Erase brush + future Replace
    // brush can find this node by world position. Asset name is the only
    // metadata we keep — grid doesn't carry kit/piece bookkeeping yet.
    GridPlacedRegistry::Get().Add(node,
                                  request.assetName ? request.assetName : "",
                                  request.position,
                                  request.rotation);

    // v9: push an EditorAction so Ctrl+Z reverses this spawn. Skip
    // during replay to avoid infinite history during redo.
    if (!sReplayingAction && api->EditorAction_Push)
    {
        auto* rec = new GridUndoRecord();
        rec->assetName = request.assetName ? request.assetName : "";
        rec->pos       = request.position;
        rec->rot       = request.rotation;
        rec->node      = node;
        api->EditorAction_Push("level-builder.grid.spawn",
                               /*do=*/   &Grid_EnsureAlive,
                               /*undo=*/ &Grid_EnsureDead,
                               /*free=*/ &Grid_FreeRecord,
                               rec);
    }

    result.success      = 1;
    result.spawnedNode  = node;
    result.errorMessage = nullptr;
    return result;
}

// EnsureAlive / EnsureDead — definitions deferred until after
// GridSpawnAtTransform exists (used by EnsureAlive's replay path).
namespace
{
    void Grid_EnsureAlive(void* p)
    {
        auto* s = (GridUndoRecord*)p;
        if (s->node) return;
        sReplayingAction = true;
        s->node = GridSpawnAtTransform(s->assetName.c_str(), &s->pos, &s->rot, nullptr);
        sReplayingAction = false;
    }
    void Grid_EnsureDead(void* p)
    {
        auto* s = (GridUndoRecord*)p;
        if (!s->node) return;

        // DEFENSIVE: see Modular_EnsureDead. If the node isn't in our
        // placed registry, it was already destroyed via some other path
        // (Replace, manual delete, engine destroy) — don't double-free.
        auto& reg = GridPlacedRegistry::Get();
        bool stillThere = false;
        for (int i = 0; i < reg.Count(); ++i)
        {
            if (reg.At(i).node == s->node) { stillThere = true; break; }
        }
        if (!stillThere)
        {
            s->node = nullptr;
            return;
        }

        PolyphaseEngineAPI* eng = EngineAPI();
        if (eng && eng->DestroyNode) eng->DestroyNode((Node*)s->node);
        reg.RemoveByNode(s->node);
        s->node = nullptr;
    }
}

// -----------------------------------------------------------------------------
// GridLevelBuilderTool
// -----------------------------------------------------------------------------

void GridLevelBuilderTool::Activate(LevelBuilderContext* /*ctx*/)
{
    LevelBuilderCoreAPI* api = CoreAPI();
    if (!api) return;
    if (api->SetActiveBrush)        api->SetActiveBrush("Grid Single");
    if (api->SetActiveSnapProvider) api->SetActiveSnapProvider("Grid (uniform)");
}

void GridLevelBuilderTool::Deactivate()
{
    LevelBuilderCoreAPI* api = CoreAPI();
    if (api && api->HidePreview) api->HidePreview();
}

void GridLevelBuilderTool::DrawSettingsUI()
{
#if EDITOR
    // Tool-level UI persists across brush changes — picking a tool-agnostic
    // brush like "Line" while in Grid Placement mode still needs the kit /
    // piece picker visible. Same content as the Grid extension tab.
    GridUI::DrawSharedSections();
#endif
}

// -----------------------------------------------------------------------------
// Viewport overlay — wire-cube grid centered on the preview position
// -----------------------------------------------------------------------------

namespace
{
    // Column-major TRS matrix from (translation, quaternion). Format
    // matches what Gizmos_SetMatrix expects (standard GL/Vulkan layout).
    void BuildTRSMatrix(const LBVec3& t, const LBQuat& q, float out[16])
    {
        float x=q.x, y=q.y, z=q.z, w=q.w;
        float xx=x*x, yy=y*y, zz=z*z;
        float xy=x*y, xz=x*z, yz=y*z;
        float wx=w*x, wy=w*y, wz=w*z;

        out[ 0] = 1.0f - 2.0f*(yy+zz);  out[ 1] = 2.0f*(xy + wz);       out[ 2] = 2.0f*(xz - wy);       out[ 3] = 0.0f;
        out[ 4] = 2.0f*(xy - wz);       out[ 5] = 1.0f - 2.0f*(xx+zz);  out[ 6] = 2.0f*(yz + wx);       out[ 7] = 0.0f;
        out[ 8] = 2.0f*(xz + wy);       out[ 9] = 2.0f*(yz - wx);       out[10] = 1.0f - 2.0f*(xx+yy);  out[11] = 0.0f;
        out[12] = t.x;                  out[13] = t.y;                  out[14] = t.z;                  out[15] = 1.0f;
    }

    // Read the active palette piece's bounding extents from core (via the
    // v3 ABI). Mirrors modular's GetActivePieceSize.
    LBVec3 GetActivePieceSize(LevelBuilderCoreAPI* api)
    {
        LBVec3 fallback{1.0f, 1.0f, 1.0f};
        if (!api || !api->GetActivePalette || !api->Kit_GetActiveIndex
              || !api->Kit_FindPieceByAsset || !api->Kit_GetPieceInfo)
            return fallback;

        LevelBuilderPalette* pal = api->GetActivePalette();
        if (!pal) return fallback;
        int idx = api->Palette_GetActiveIndex(pal);
        if (idx < 0) return fallback;
        LevelBuilderPaletteItem it{};
        if (!api->Palette_GetItem(pal, idx, &it)) return fallback;
        if (!it.assetName) return fallback;

        int kitIdx = api->Kit_GetActiveIndex();
        if (kitIdx < 0) return fallback;
        int pieceIdx = api->Kit_FindPieceByAsset(kitIdx, it.assetName);
        if (pieceIdx < 0) return fallback;

        LBPieceInfo pi{};
        if (!api->Kit_GetPieceInfo(kitIdx, pieceIdx, &pi)) return fallback;

        LBVec3 s{pi.size[0], pi.size[1], pi.size[2]};
        if (s.x <= 0.0f) s.x = 1.0f;
        if (s.y <= 0.0f) s.y = 1.0f;
        if (s.z <= 0.0f) s.z = 1.0f;
        return s;
    }

    void DrawGridOverlay_Impl(float /*x*/, float /*y*/, float /*w*/, float /*h*/, void* /*ud*/)
    {
        if (!sOverlayOn) return;

        LevelBuilderCoreAPI* api = CoreAPI();
        if (!api || !api->GetActiveToolName || !api->GetEngineAPI) return;

        // Only paint when the Grid tool is the active one — otherwise the
        // user could be using modular and we'd litter their viewport with
        // alien wireframes.
        const char* activeTool = api->GetActiveToolName();
        if (!activeTool || std::strcmp(activeTool, "Grid Placement") != 0)
            return;

        PolyphaseEngineAPI* eng = (PolyphaseEngineAPI*)api->GetEngineAPI();
        if (!eng || !eng->Gizmos_DrawWireCube) return;

        // Center the grid on the preview's snapped position when there is
        // a live preview; otherwise fall back to the grid origin. This
        // keeps the overlay localized to where the user is working without
        // needing a camera-position query (the plugin API doesn't expose
        // one today).
        bool havePreview = false;
        LBVec3 center{sOriginX, sOriginY, sOriginZ};
        LBVec3 rawPos{sOriginX, sOriginY, sOriginZ};
        LBQuat previewRot{0, 0, 0, 1};
        int previewState = (int)LBPreview_Valid;
        if (api->GetPreviewState && api->GetPreviewState() != LBPreview_Hidden &&
            api->GetPreviewTransform)
        {
            havePreview = true;
            previewState = api->GetPreviewState();
            LBVec3 pos, scl;
            api->GetPreviewTransform(&pos, &previewRot, &scl);
            rawPos = pos;
            center.x = SnapAxis(pos.x, sOriginX, sCellX);
            center.y = SnapAxis(pos.y, sOriginY, sCellY);
            center.z = SnapAxis(pos.z, sOriginZ, sCellZ);
        }

        const int N = sOverlayRadius > 0 ? sOverlayRadius : 0;

        // ---- 1. lattice wire cubes -----------------------------------
        if (eng->Gizmos_SetColor) eng->Gizmos_SetColor(0.45f, 0.65f, 0.90f, 0.30f);
        for (int dz = -N; dz <= N; ++dz)
        {
            for (int dx = -N; dx <= N; ++dx)
            {
                if (dx == 0 && dz == 0)
                {
                    if (eng->Gizmos_SetColor) eng->Gizmos_SetColor(0.20f, 1.00f, 0.30f, 0.85f);
                }
                float cx = center.x + dx * sCellX;
                float cz = center.z + dz * sCellZ;
                eng->Gizmos_DrawWireCube(cx, center.y, cz, sCellX, sCellY, sCellZ);
                if (dx == 0 && dz == 0)
                {
                    if (eng->Gizmos_SetColor) eng->Gizmos_SetColor(0.45f, 0.65f, 0.90f, 0.30f);
                }
            }
        }

        // ---- 2. piece-bounds wire cube (rotates with the preview) ----
        // The lattice above is axis-aligned (cells never rotate); the
        // piece bbox rotates so it matches how the spawned mesh will
        // actually sit. We use Gizmos_SetMatrix to apply the preview's
        // rotation, then draw the cube at local-space (0, half-height, 0)
        // so the bbox floats on the floor exactly the way the piece will.
        if (havePreview && eng->Gizmos_SetMatrix)
        {
            LBVec3 size = GetActivePieceSize(api);

            switch (previewState)
            {
            case LBPreview_Valid:        eng->Gizmos_SetColor(0.20f, 1.00f, 0.30f, 0.85f); break;
            case LBPreview_Invalid:      eng->Gizmos_SetColor(1.00f, 0.25f, 0.25f, 0.85f); break;
            case LBPreview_Overlapping:  eng->Gizmos_SetColor(1.00f, 0.55f, 0.10f, 0.85f); break;
            case LBPreview_MissingAsset: eng->Gizmos_SetColor(0.50f, 0.50f, 0.50f, 0.85f); break;
            default:                     eng->Gizmos_SetColor(1.00f, 1.00f, 1.00f, 0.60f); break;
            }

            float M[16];
            BuildTRSMatrix(center, previewRot, M);
            eng->Gizmos_SetMatrix(M);
            eng->Gizmos_DrawWireCube(0.0f, 0.5f * size.y, 0.0f, size.x, size.y, size.z);
            (void)rawPos;  // reserved for future "show drift between raw and snapped" debug viz
        }

        if (eng->Gizmos_ResetState) eng->Gizmos_ResetState();
    }
}

extern "C" void GridPlacement_DrawViewportOverlayTrampoline(
    float x, float y, float w, float h, void* userData)
{
    DrawGridOverlay_Impl(x, y, w, h, userData);
}

// -----------------------------------------------------------------------------
// GridPlacement namespace — init / refresh / placement / settings
// -----------------------------------------------------------------------------

void GridPlacement::Initialize()
{
    if (!sSnap)  sSnap  = new GridSnapProvider();
    if (!sBrush) sBrush = new GridPlacementBrush();
    if (!sTool)  sTool  = new GridLevelBuilderTool();

    LevelBuilderCoreAPI* api = CoreAPI();
    if (!api) return;

    api->RegisterSnapProvider("Grid (uniform)", sSnap);
    api->RegisterBrush       ("Grid Single",    sBrush);
    api->RegisterTool        ("Grid Placement", sTool);

    // v2 ABI: subscribe to viewport click events so a left-click in the
    // viewport while "Grid Placement" is active commits the preview.
    // Modular wires this same way (see ModularPlacement::Initialize).
    if (api->RegisterToolViewportInput)
    {
        api->RegisterToolViewportInput("Grid Placement",
                                       &GridPlacement_OnViewportClick,
                                       nullptr);
    }

    // v4 ABI (Phase T5): register the spawn fn that tool.core's generic
    // brushes call to place grid pieces. Keyed by tool name so the right
    // sibling handler fires when "Grid Placement" is the active tool.
    if (api->RegisterEnumerateFn)
    {
        api->RegisterEnumerateFn("Grid Placement",
                                 &GridEnumeratePlacements,
                                 nullptr);
    }

    // v13: register the rebuild-from-world callback so core can rescan
    // the scene on Level Builder mode activate.
    if (api->RegisterRebuildFromWorldFn)
    {
        api->RegisterRebuildFromWorldFn("Grid Placement",
            [](void* /*ud*/) { GridPlacedRegistry::Get().RebuildFromWorld(); },
            nullptr);
    }
    if (api->RegisterSpawnFn)
    {
        api->RegisterSpawnFn("Grid Placement",
                             &GridSpawnAtTransform,
                             nullptr);
    }

    // R2: core owns the kit registry now. Its OnLoad already called
    // EnsureBuiltinKit + ScanProjectKits before grid's OnLoad runs (we
    // depend on core in package.json). Just point the grid palette at
    // core's active kit.
    RefreshPaletteForActiveKit();
}

void GridPlacement::Shutdown()
{
#if EDITOR
    ThumbnailCache::Clear();
#endif

    LevelBuilderCoreAPI* api = CoreAPI();
    if (api)
    {
        // Drop the spawn-fn FIRST so any in-flight Line/Box commit from
        // tool.core can't fire our callback after sBrush is gone.
        if (api->UnregisterSpawnFn)
            api->UnregisterSpawnFn("Grid Placement");
        if (api->UnregisterEnumerateFn)
            api->UnregisterEnumerateFn("Grid Placement");
        if (api->UnregisterRebuildFromWorldFn)
            api->UnregisterRebuildFromWorldFn("Grid Placement");
        if (api->UnregisterToolViewportInput)
            api->UnregisterToolViewportInput("Grid Placement");
        api->UnregisterTool        ("Grid Placement");
        api->UnregisterBrush       ("Grid Single");
        api->UnregisterSnapProvider("Grid (uniform)");

        // Drop our palette so core doesn't keep stale pointers into our
        // soon-to-be-unloaded strings.
        if (api->Kit_GetActiveName)
        {
            const char* activeName = api->Kit_GetActiveName();
            if (activeName && *activeName)
            {
                std::string paletteName = std::string("Grid: ") + activeName;
                api->UnregisterPalette(paletteName.c_str());
            }
        }
    }

    delete sTool;  sTool  = nullptr;
    delete sBrush; sBrush = nullptr;
    delete sSnap;  sSnap  = nullptr;
}

void GridPlacement::RefreshPaletteForActiveKit()
{
    LevelBuilderCoreAPI* api = CoreAPI();
    if (!api) return;
    if (!api->Kit_GetActiveIndex || !api->Kit_GetInfo || !api->Kit_GetPieceInfo)
        return;

    int kitIdx = api->Kit_GetActiveIndex();
    if (kitIdx < 0) return;
    LBKitInfo ki{};
    if (!api->Kit_GetInfo(kitIdx, &ki)) return;

    std::string paletteName = std::string("Grid: ") + (ki.name ? ki.name : "");
    LevelBuilderPalette* pal = api->CreatePalette(paletteName.c_str());
    if (!pal) return;
    api->Palette_Clear(pal);

    for (int i = 0; i < ki.pieceCount; ++i)
    {
        LBPieceInfo pi{};
        if (!api->Kit_GetPieceInfo(kitIdx, i, &pi)) continue;

        LevelBuilderPaletteItem it;
        it.displayName = pi.name;
        it.assetName   = pi.assetName;
        it.category    = pi.category;
        it.iconPath    = pi.iconPath;
        it.tags        = pi.name;
        api->Palette_AddItem(pal, &it);
    }
    api->SetActivePalette(paletteName.c_str());
}

void GridPlacement::PlaceFromPreview()
{
    LevelBuilderCoreAPI* api = CoreAPI();
    if (!api) return;
    LevelBuilderPalette* pal = api->GetActivePalette();
    if (!pal) return;
    int idx = api->Palette_GetActiveIndex(pal);
    if (idx < 0) return;
    LevelBuilderPaletteItem it;
    if (!api->Palette_GetItem(pal, idx, &it)) return;

    LBVec3 pos, scl; LBQuat rot;
    api->GetPreviewTransform(&pos, &rot, &scl);
    if (scl.x == 0 && scl.y == 0 && scl.z == 0) scl = LBVec3{1, 1, 1};

    // Force-snap whatever the preview has — even if the user dragged the
    // raw World Pos field — so placements always land on the grid.
    pos.x = SnapAxis(pos.x, sOriginX, sCellX);
    pos.y = SnapAxis(pos.y, sOriginY, sCellY);
    pos.z = SnapAxis(pos.z, sOriginZ, sCellZ);

    LevelBuilderPlacementRequest req{};
    req.assetName  = it.assetName;
    req.position   = pos;
    req.rotation   = rot;
    req.scale      = scl;
    req.parentNode = nullptr;

    LevelBuilderPlacementResult res = api->Place(&req);
    if (!res.success && res.errorMessage)
        api->LogWarning(res.errorMessage);
}

// Fires when the user left-clicks inside the viewport while "Grid
// Placement" is active. Core's per-frame raycast has already updated the
// preview transform via GridSnapProvider; we just commit.
static void GridPlacement_OnViewportClick(const LBVec3* hitPos,
                                          const LBVec3* /*hitNormal*/,
                                          void*         /*hitNode*/,
                                          int           /*button*/,
                                          void*         /*userData*/)
{
    // If the active brush opts out of needing an armed palette item
    // (Replace etc.), route the click directly through api->Place with
    // the raw hit position — bypassing the PlaceFromPreview path which
    // requires the palette/preview to be armed.
    LevelBuilderCoreAPI* api = CoreAPI();
    if (api && api->FindBrush && api->GetActiveBrushName)
    {
        const char* activeBrushName = api->GetActiveBrushName();
        LevelBuilderBrush* activeBrush = activeBrushName ? api->FindBrush(activeBrushName) : nullptr;
        if (activeBrush && !activeBrush->NeedsArmedPreview())
        {
            LevelBuilderPlacementRequest req{};
            req.assetName  = "";
            req.position   = hitPos ? *hitPos : LBVec3{0,0,0};
            req.rotation   = LBQuat{0, 0, 0, 1};
            req.scale      = LBVec3{1, 1, 1};
            req.parentNode = nullptr;
            if (api->Place) api->Place(&req);
            return;
        }
    }

    GridPlacement::PlaceFromPreview();
}

void GridPlacement::TickEditor(float /*deltaTime*/)
{
    // Gate on "Grid Placement" active tool so the hotkey doesn't fire
    // simultaneously with modular's R handler when the user is in
    // modular mode. Each sibling owns its own yaw — same preview slot,
    // exclusive ownership at the active-tool granularity.
    LevelBuilderCoreAPI* api = CoreAPI();
    if (!api || !api->GetActiveToolName) return;

    const char* active = api->GetActiveToolName();
    if (!active || std::strcmp(active, "Grid Placement") != 0)
        return;

#if EDITOR
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;

    if (ImGui::IsKeyPressed(ImGuiKey_R, /*repeat=*/false))
    {
        // -90° clockwise (matches modular's R semantics). Snap provider
        // quantizes per frame to sYawStepDeg, so non-90 yaw steps still
        // land on the user's configured snap step.
        AddYawDelta(-90.0f);

        if (api->LogDebug)
        {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "[Grid/R] yaw -> %.1f", sGridYawDeg);
            api->LogDebug(buf);
        }
    }
#endif
}

float GridPlacement::GetYawDeg()                { return sGridYawDeg; }
void  GridPlacement::SetYawDeg(float deg)
{
    sGridYawDeg = WrapYaw(deg);
    ApplyGridYawToPreview();
}
void  GridPlacement::AddYawDelta(float deg)
{
    sGridYawDeg = WrapYaw(sGridYawDeg + deg);
    ApplyGridYawToPreview();
}

// ---- settings accessors ----

void  GridPlacement::SetCellSize(float x, float y, float z)
{
    sCellX = x > 0.0001f ? x : 0.0001f;
    sCellY = y > 0.0001f ? y : 0.0001f;
    sCellZ = z > 0.0001f ? z : 0.0001f;
}
void  GridPlacement::GetCellSize(float* outX, float* outY, float* outZ)
{
    if (outX) *outX = sCellX;
    if (outY) *outY = sCellY;
    if (outZ) *outZ = sCellZ;
}
void  GridPlacement::SetYawStepDeg(float deg) { sYawStepDeg = deg < 0.0f ? 0.0f : deg; }
float GridPlacement::GetYawStepDeg()          { return sYawStepDeg; }

void  GridPlacement::SetGridOrigin(float x, float y, float z)
{
    sOriginX = x; sOriginY = y; sOriginZ = z;
}
void  GridPlacement::GetGridOrigin(float* outX, float* outY, float* outZ)
{
    if (outX) *outX = sOriginX;
    if (outY) *outY = sOriginY;
    if (outZ) *outZ = sOriginZ;
}

void GridPlacement::SetOverlayEnabled(bool e) { sOverlayOn = e; }
bool GridPlacement::GetOverlayEnabled()       { return sOverlayOn; }
void GridPlacement::SetOverlayRadius(int n)   { sOverlayRadius = n < 0 ? 0 : n; }
int  GridPlacement::GetOverlayRadius()        { return sOverlayRadius; }
