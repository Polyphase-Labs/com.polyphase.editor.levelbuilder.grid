/**
 * @file ComPolyphaseEditorLevelbuilderGrid.cpp
 * @brief Entry point for com.polyphase.editor.levelbuilder.grid.
 *
 * On load:
 *   - Bootstrap the addon's ImGui context so editor-side UI works.
 *   - Resolve the core addon's API (late-bound via GetProcAddress).
 *   - Register the grid tool/brush/snap provider with core.
 *   - Add a "Grid" tab to the Level Builder window + a wire-cube viewport
 *     overlay.
 *
 * On unload (hot-reload safe):
 *   - Unregister tab + overlay + tool/brush/snap from core.
 *   - Drop the cached core API pointer.
 */

#include "Plugins/PolyphasePluginAPI.h"
#include "Plugins/PolyphaseEngineAPI.h"

#if EDITOR
#include "Plugins/EditorUIHooks.h"
#include "Plugins/ImGuiPluginContext.h"
#include "imgui.h"
#endif

#include "GridPlacement.h"
#include "LevelBuilderCoreLoader.h"

#if EDITOR
#include "GridUI.h"
#endif

static PolyphaseEngineAPI* sEngineAPI = nullptr;
#if EDITOR
static EditorUIHooks*      sHooks     = nullptr;
static uint64_t            sHookId    = 0;
#endif

// Plugin-descriptor trampoline for per-frame editor tick — forwards to
// GridPlacement::TickEditor which handles the R-key rotate hotkey.
// Matches the shape modular wires for its own ModularTickEditor.
static void GridTickEditor(float deltaTime)
{
    GridPlacement::TickEditor(deltaTime);
}

static int OnLoad(PolyphaseEngineAPI* api)
{
    sEngineAPI = api;

#if EDITOR
    if (api && api->GetImGuiContext)
    {
        ImGuiPluginContext ctx{};
        api->GetImGuiContext(&ctx);
        if (ctx.context)
        {
            ImGui::SetCurrentContext(ctx.context);
            ImGui::SetAllocatorFunctions(ctx.allocFunc, ctx.freeFunc, ctx.allocUserData);
        }
    }
#endif

    if (!LevelBuilderCoreLoader::Get())
    {
        if (api && api->LogWarning)
            api->LogWarning("[LevelBuilderGrid] core addon not loaded yet — "
                            "ensure com.polyphase.editor.levelbuilder.core is loaded first");
        return 0;  // graceful: a hot-reload will retry
    }

    GridPlacement::Initialize();

    if (api && api->LogDebug)
        api->LogDebug("[LevelBuilderGrid] loaded");

    return 0;
}

static void OnUnload()
{
#if EDITOR
    if (sHooks)
        GridUI::UnregisterViewportOverlay(sHooks, sHookId);
    GridUI::Unregister();
#endif

    GridPlacement::Shutdown();
    LevelBuilderCoreLoader::Reset();

    if (sEngineAPI && sEngineAPI->LogDebug)
        sEngineAPI->LogDebug("[LevelBuilderGrid] unloaded");

    sEngineAPI = nullptr;
#if EDITOR
    sHooks  = nullptr;
    sHookId = 0;
#endif
}

static void RegisterTypes(void* /*nodeFactory*/) {}

static void RegisterScriptFuncs(struct lua_State* /*L*/) {}

#if EDITOR
static void RegisterEditorUI(EditorUIHooks* hooks, uint64_t hookId)
{
    sHooks  = hooks;
    sHookId = hookId;
    GridUI::Register();
    GridUI::RegisterViewportOverlay(hooks, hookId);
}
#endif

extern "C" OCTAVE_PLUGIN_API int PolyphasePlugin_GetDesc(PolyphasePluginDesc* desc)
{
    desc->apiVersion          = OCTAVE_PLUGIN_API_VERSION;
    desc->pluginName          = "com.polyphase.editor.levelbuilder.grid";
    desc->pluginVersion       = "0.1.0";
    desc->OnLoad              = OnLoad;
    desc->OnUnload            = OnUnload;
    desc->Tick                = nullptr;
    desc->TickEditor          = &GridTickEditor;
    desc->RegisterTypes       = RegisterTypes;
    desc->RegisterScriptFuncs = RegisterScriptFuncs;
#if EDITOR
    desc->RegisterEditorUI    = RegisterEditorUI;
#else
    desc->RegisterEditorUI    = nullptr;
#endif
    desc->OnEditorPreInit     = nullptr;
    desc->OnEditorReady       = nullptr;
    return 0;
}
