#include "GridUI.h"

#if EDITOR

#include "GridPlacement.h"
#include "LevelBuilderCoreLoader.h"
#include "LevelBuilderInterfaces.h"
#include "ThumbnailCache.h"

#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    LevelBuilderCoreAPI* CoreAPI() { return LevelBuilderCoreLoader::Get(); }

    std::vector<std::string> sKitLoadErrors;
    int                      sLastReloadCount = -1;

    char sProjectRootInput[512]   = {0};
    bool sShowProjectRootInput    = false;

    // ---- Kit section: modular-style dropdown, Reload, project-root override.

    void DrawKitSection()
    {
        LevelBuilderCoreAPI* api = CoreAPI();
        if (!api) { ImGui::TextDisabled("Core API unavailable."); return; }
        // Header row is now the parent CollapsingHeader ("Kit").

        const char* activeName = api->Kit_GetActiveName ? api->Kit_GetActiveName() : "";
        const char* preview    = (activeName && *activeName) ? activeName : "<none>";
        int kitCount = api->Kit_GetCount ? api->Kit_GetCount() : 0;

        ImGui::SetNextItemWidth(180);
        if (ImGui::BeginCombo("##grid_ui_kit", preview))
        {
            for (int i = 0; i < kitCount; ++i)
            {
                const char* n = api->Kit_GetNameAt(i);
                bool sel = activeName && std::strcmp(n, activeName) == 0;
                if (ImGui::Selectable(n, sel))
                {
                    api->Kit_SetActiveByName(n);
                    GridPlacement::RefreshPaletteForActiveKit();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Refresh"))
            GridPlacement::RefreshPaletteForActiveKit();

        ImGui::Spacing();
        const char* kitsFolder = api->GetProjectKitsFolder ? api->GetProjectKitsFolder() : "";
        if (!kitsFolder || !*kitsFolder)
        {
            ImGui::TextColored(ImVec4(1, 0.55f, 0.10f, 1),
                "Kits folder: <not found — couldn't auto-detect project root>");
        }
        else
        {
            ImGui::TextDisabled("Kits folder: %s", kitsFolder);
        }

        if (ImGui::Button("Reload Kits From Disk"))
        {
            if (api->ClearProjectRootCache) api->ClearProjectRootCache();
            if (api->Kit_Reload) api->Kit_Reload();

            sKitLoadErrors.clear();
            if (api->Kit_GetLastReloadErrorCount && api->Kit_GetLastReloadError)
            {
                int n = api->Kit_GetLastReloadErrorCount();
                for (int i = 0; i < n; ++i)
                {
                    const char* e = api->Kit_GetLastReloadError(i);
                    if (e) sKitLoadErrors.emplace_back(e);
                }
            }
            sLastReloadCount = api->Kit_GetLastReloadKitCount
                                ? api->Kit_GetLastReloadKitCount() : 0;
            GridPlacement::RefreshPaletteForActiveKit();
        }
        ImGui::SameLine();
        if (ImGui::Button("Copy Path") && kitsFolder && *kitsFolder)
            ImGui::SetClipboardText(kitsFolder);
        ImGui::SameLine();
        if (ImGui::Button(sShowProjectRootInput ? "Hide Override" : "Set Project Root…"))
            sShowProjectRootInput = !sShowProjectRootInput;

        if (sShowProjectRootInput)
        {
            ImGui::SetNextItemWidth(420);
            ImGui::InputTextWithHint("##grid_proj_root_override",
                                     "Project root path (e.g. M:\\Projects\\Polyphase\\Addons\\GridWorld)",
                                     sProjectRootInput, sizeof(sProjectRootInput));
            ImGui::SameLine();
            if (ImGui::Button("Apply##grid_apply_root"))
            {
                if (api->SetProjectRootOverride) api->SetProjectRootOverride(sProjectRootInput);
                if (api->Kit_Reload) api->Kit_Reload();
                GridPlacement::RefreshPaletteForActiveKit();
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear##grid_clear_root"))
            {
                if (api->SetProjectRootOverride) api->SetProjectRootOverride(nullptr);
                sProjectRootInput[0] = 0;
            }
            ImGui::TextDisabled(
                "Tip: paste the folder that contains your Kits/ folder. "
                "Or set POLYPHASE_PROJECT in your environment.");
        }

        if (sLastReloadCount >= 0)
            ImGui::TextDisabled("Last reload: %d kit(s) loaded, %d error(s)",
                                sLastReloadCount, (int)sKitLoadErrors.size());
        for (const auto& e : sKitLoadErrors)
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", e.c_str());
    }

    // ---- Piece grid: modular-style thumbnail cards (96px) ----------------

    const std::string& CachedProjectRoot()
    {
        static std::string sRoot;
        LevelBuilderCoreAPI* api = CoreAPI();
        if (api && api->GetProjectRoot)
        {
            const char* p = api->GetProjectRoot();
            if (p && *p) { sRoot = p; return sRoot; }
        }
        sRoot.clear();
        return sRoot;
    }

    // Resolve a piece's `icon` to an absolute path. Tries kit-folder-relative
    // first (canonical for folder-mode kits) then falls back to
    // project-root-relative (legacy + loose-format kits). See the same
    // helper in modular's ModularUI.cpp for full details.
    std::string ResolveIconAbs(const char* iconPath,
                               const std::string& projectRoot,
                               const std::string& kitFolder)
    {
        if (!iconPath || !*iconPath) return {};
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path ip(iconPath);
        if (ip.is_absolute()) return iconPath;
        if (!kitFolder.empty())
        {
            fs::path a = fs::path(kitFolder) / iconPath;
            if (fs::is_regular_file(a, ec)) return a.string();
        }
        if (!projectRoot.empty())
        {
            fs::path a = fs::path(projectRoot) / iconPath;
            if (fs::is_regular_file(a, ec)) return a.string();
            return a.string();
        }
        return iconPath;
    }

    std::string GetActiveKitFolder(LevelBuilderCoreAPI* api)
    {
        if (!api || !api->Kit_GetActiveIndex || !api->Kit_GetInfo) return {};
        int kitIdx = api->Kit_GetActiveIndex();
        if (kitIdx < 0) return {};
        LBKitInfo ki{};
        if (!api->Kit_GetInfo(kitIdx, &ki)) return {};
        if (!ki.sourceFile || !*ki.sourceFile) return {};
        return std::filesystem::path(ki.sourceFile).parent_path().string();
    }

    void DrawPieceCard(LevelBuilderCoreAPI* api,
                       LevelBuilderPalette* pal,
                       const LevelBuilderPaletteItem& it,
                       int index, int activeIdx, float cardW)
    {
        ImTextureID texId = 0;
        if (it.iconPath && *it.iconPath)
        {
            const std::string& root = CachedProjectRoot();
            std::string kitFolder = GetActiveKitFolder(api);
            std::string abs = ResolveIconAbs(it.iconPath, root, kitFolder);
            if (!abs.empty())
                texId = ThumbnailCache::Get(abs);
        }

        char idBuf[32];
        std::snprintf(idBuf, sizeof(idBuf), "##grid_card_%d", index);

        const bool isActive = (activeIdx == index);
        if (isActive)
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.30f, 0.65f, 1.00f, 1.0f));

        ImGui::BeginGroup();

        bool clicked = false;
        if (texId != 0)
        {
            clicked = ImGui::ImageButton(idBuf, texId, ImVec2(cardW, cardW));
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.25f, 0.25f, 0.27f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f, 0.32f, 0.36f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.40f, 0.40f, 0.44f, 1.0f));
            ImVec2 btnTopLeft = ImGui::GetCursorScreenPos();
            clicked = ImGui::Button(idBuf, ImVec2(cardW, cardW));
            ImGui::PopStyleColor(3);

            const char* label = it.displayName ? it.displayName : "?";
            ImVec2 textSize = ImGui::CalcTextSize(label);
            ImVec2 textPos(btnTopLeft.x + (cardW - textSize.x) * 0.5f,
                           btnTopLeft.y + (cardW - textSize.y) * 0.5f);
            if (textPos.x < btnTopLeft.x + 4) textPos.x = btnTopLeft.x + 4;
            ImGui::GetWindowDrawList()->AddText(textPos,
                                                ImGui::GetColorU32(ImGuiCol_Text),
                                                label);
        }

        if (clicked)
        {
            api->Palette_SetActiveIndex(pal, index);
            if (api->SetPreviewAsset)  api->SetPreviewAsset(it.assetName);
            if (api->SetPreviewState)  api->SetPreviewState(LBPreview_Valid);
        }
        if (ImGui::IsItemHovered() && it.displayName && *it.displayName)
            ImGui::SetTooltip("%s%s%s",
                              it.displayName,
                              (it.category && *it.category) ? "\n" : "",
                              (it.category && *it.category) ? it.category : "");

        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cardW);
        ImGui::TextUnformatted(it.displayName ? it.displayName : "?");
        if (it.category && *it.category)
            ImGui::TextDisabled("[%s]", it.category);
        ImGui::PopTextWrapPos();

        ImGui::EndGroup();

        if (isActive)
            ImGui::PopStyleColor();
    }

    void DrawPieceSection()
    {
        // Header row is now the parent CollapsingHeader ("Pieces").

        LevelBuilderCoreAPI* api = CoreAPI();
        if (!api) { ImGui::TextDisabled("Core API unavailable."); return; }

        LevelBuilderPalette* pal = api->GetActivePalette();
        if (!pal)
        {
            ImGui::TextDisabled("No active palette — pick a kit above and hit Refresh.");
            return;
        }
        int n = api->Palette_GetItemCount(pal);
        int activeIdx = api->Palette_GetActiveIndex(pal);
        if (n == 0)
        {
            ImGui::TextDisabled("Active kit has no pieces.");
            return;
        }

        ImGui::BeginChild("##grid_piece_grid", ImVec2(0.0f, 320.0f), true,
                          ImGuiWindowFlags_HorizontalScrollbar);

        const float kCardSize    = 96.0f;
        const float kCardSpacing = 12.0f;
        const float avail        = ImGui::GetContentRegionAvail().x;
        int   perRow             = (int)(avail / (kCardSize + kCardSpacing));
        if (perRow < 1) perRow = 1;

        for (int i = 0; i < n; ++i)
        {
            LevelBuilderPaletteItem it{};
            if (!api->Palette_GetItem(pal, i, &it)) continue;
            DrawPieceCard(api, pal, it, i, activeIdx, kCardSize);
            if (((i + 1) % perRow) != 0 && i + 1 < n)
                ImGui::SameLine(0.0f, kCardSpacing);
        }
        ImGui::EndChild();
    }

    // ---- Placement section: modular-style World Pos drag + Place Here ----

    void DrawPlacementSection()
    {
        // Header row is now the parent CollapsingHeader ("Placement").

        LevelBuilderCoreAPI* api = CoreAPI();
        if (!api) return;

        ImGui::TextDisabled("Click in the viewport to place at the hover hit,");
        ImGui::TextDisabled("or dial coordinates manually below.");

        LBVec3 curPos, curScl; LBQuat curRot;
        api->GetPreviewTransform(&curPos, &curRot, &curScl);

        static float sPos[3]     = {0,0,0};
        static bool  sDragActive = false;
        if (!sDragActive)
        {
            sPos[0] = curPos.x;
            sPos[1] = curPos.y;
            sPos[2] = curPos.z;
        }

        bool changed = ImGui::DragFloat3("World Pos", sPos, 0.1f);
        sDragActive = ImGui::IsItemActive();

        if (changed)
        {
            // Snap manually-dialed positions through Grid (uniform) so the
            // preview and the eventual placement land on the same cell.
            LBVec3 raw{sPos[0], sPos[1], sPos[2]};
            LBVec3 newPos = raw;
            LBQuat newRot = curRot;
            LevelBuilderSnapProvider* sp = api->FindSnapProvider("Grid (uniform)");
            if (sp)
                sp->GetSnapTransform(raw, newPos, newRot);
            api->SetPreviewTransform(&newPos, &newRot, &curScl);
        }
        api->SetPreviewState(LBPreview_Valid);

        if (ImGui::Button("Snap to Nearest Cell"))
        {
            LevelBuilderSnapProvider* sp = api->FindSnapProvider("Grid (uniform)");
            if (sp)
            {
                LBVec3 snappedPos = curPos;
                LBQuat snappedRot = curRot;
                if (sp->GetSnapTransform(curPos, snappedPos, snappedRot))
                {
                    api->SetPreviewTransform(&snappedPos, &snappedRot, &curScl);
                    sPos[0] = snappedPos.x;
                    sPos[1] = snappedPos.y;
                    sPos[2] = snappedPos.z;
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Place Here"))
            GridPlacement::PlaceFromPreview();
    }

    // ---- Grid-specific: cell size / origin / yaw step --------------------

    void DrawSnapSection()
    {
        // Header row is now the parent CollapsingHeader ("Snap").

        float cell[3];
        GridPlacement::GetCellSize(&cell[0], &cell[1], &cell[2]);
        if (ImGui::DragFloat3("Cell Size", cell, 0.05f, 0.01f, 100.0f, "%.3f"))
            GridPlacement::SetCellSize(cell[0], cell[1], cell[2]);

        static float sUniform = 1.0f;
        ImGui::SetNextItemWidth(120);
        ImGui::DragFloat("Uniform", &sUniform, 0.05f, 0.01f, 100.0f, "%.3f");
        ImGui::SameLine();
        if (ImGui::Button("Apply Uniform"))
            GridPlacement::SetCellSize(sUniform, sUniform, sUniform);

        float origin[3];
        GridPlacement::GetGridOrigin(&origin[0], &origin[1], &origin[2]);
        if (ImGui::DragFloat3("Origin", origin, 0.05f, -1000.0f, 1000.0f, "%.3f"))
            GridPlacement::SetGridOrigin(origin[0], origin[1], origin[2]);

        float yawStep = GridPlacement::GetYawStepDeg();
        if (ImGui::DragFloat("Yaw Step (deg)", &yawStep, 1.0f, 0.0f, 360.0f, "%.1f"))
            GridPlacement::SetYawStepDeg(yawStep);
    }

    void DrawOverlaySection()
    {
        // Header row is now the parent CollapsingHeader ("Overlay").

        bool on = GridPlacement::GetOverlayEnabled();
        if (ImGui::Checkbox("Show Grid Overlay", &on))
            GridPlacement::SetOverlayEnabled(on);

        int radius = GridPlacement::GetOverlayRadius();
        ImGui::SetNextItemWidth(160);
        if (ImGui::DragInt("Radius (cells)", &radius, 0.25f, 0, 32))
            GridPlacement::SetOverlayRadius(radius);
        ImGui::SameLine();
        ImGui::TextDisabled("(%d cells drawn)", (2 * (radius < 0 ? 0 : radius) + 1)
                                                * (2 * (radius < 0 ? 0 : radius) + 1));
    }

    // ---- Debug accordion contents — placed-registry inspect/manage. ----
    // Mirrors modular's Debug controls so Paint Erase / Replace work
    // against pieces loaded from disk (project save+reload) — call
    // "Rebuild From World" once after opening the project and the
    // placed registry repopulates from existing scene nodes.
    void DrawDebugSection()
    {
        ImGui::Text("Placed pieces: %d", GridPlacedRegistry::Get().Count());
        if (ImGui::Button("Clear Placed Registry"))
            GridPlacedRegistry::Get().Clear();
        ImGui::SameLine();
        if (ImGui::Button("Rebuild From World"))
            GridPlacedRegistry::Get().RebuildFromWorld();
        ImGui::TextDisabled(
            "Rebuild scans the current scene for nodes matching any kit "
            "piece. Run after loading a project so Paint Erase / Replace "
            "see pre-existing placements.");
    }

    void DrawTab(void* /*userData*/)
    {
        GridUI::DrawSharedSections();
    }
}

namespace GridUI
{
    void DrawSharedSections()
    {
        // Each section is collapsible. Kit + Pieces default-open since
        // they're what artists touch most. Section functions no longer
        // emit their own header rows — the CollapsingHeader replaces them.
        const ImGuiTreeNodeFlags kOpen   = ImGuiTreeNodeFlags_DefaultOpen;
        const ImGuiTreeNodeFlags kClosed = 0;

        if (ImGui::CollapsingHeader("Kit",       kOpen))   DrawKitSection();
        if (ImGui::CollapsingHeader("Pieces",    kOpen))   DrawPieceSection();
        if (ImGui::CollapsingHeader("Placement", kClosed)) DrawPlacementSection();
        if (ImGui::CollapsingHeader("Snap",      kClosed)) DrawSnapSection();
        if (ImGui::CollapsingHeader("Overlay",   kClosed)) DrawOverlaySection();
        if (ImGui::CollapsingHeader("Debug",     kClosed)) DrawDebugSection();
    }

    void Register()
    {
        // The standalone "Grid" extension tab was retired — everything it
        // used to host (kit picker, piece browser, cell-size, overlay)
        // now lives in the Brush tab via GridLevelBuilderTool::DrawSettingsUI
        // → DrawSharedSections(). The extension tab was a pure duplicate.
        // DrawTab is kept around in case we ever want to bring back a
        // grid-only authoring surface as a different tab.
    }

    void Unregister()
    {
        // Symmetric no-op. UnregisterExtensionTab is still called below
        // to clean up any stale entry from older builds that did register.
        LevelBuilderCoreAPI* api = CoreAPI();
        if (!api || !api->UnregisterExtensionTab) return;
        api->UnregisterExtensionTab("Grid");
    }

    void RegisterViewportOverlay(EditorUIHooks* hooks, uint64_t hookId)
    {
        if (!hooks || !hooks->RegisterViewportOverlay) return;
        hooks->RegisterViewportOverlay(hookId, "GridOverlay",
                                       &GridPlacement_DrawViewportOverlayTrampoline,
                                       nullptr);
    }

    void UnregisterViewportOverlay(EditorUIHooks* hooks, uint64_t hookId)
    {
        if (!hooks || !hooks->UnregisterViewportOverlay) return;
        hooks->UnregisterViewportOverlay(hookId, "GridOverlay");
    }
}

#endif // EDITOR
