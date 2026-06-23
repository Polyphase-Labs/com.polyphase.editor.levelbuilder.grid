/**
 * @file GridUI.h
 * @brief ImGui Grid extension tab inside the Level Builder window.
 */

#pragma once

#if EDITOR

#include "Plugins/EditorUIHooks.h"

namespace GridUI
{
    // Registers the "Grid" extension tab with the core via
    // RegisterExtensionTab. Idempotent.
    void Register();

    // Unregisters the tab. Called from the addon's OnUnload.
    void Unregister();

    // Registers the grid viewport overlay with EditorUIHooks so the
    // wire-cube grid is painted each frame the Grid tool is active.
    void RegisterViewportOverlay(EditorUIHooks* hooks, uint64_t hookId);
    void UnregisterViewportOverlay(EditorUIHooks* hooks, uint64_t hookId);

    // The modular-style kit / piece / placement / snap / overlay UI.
    // Called from both the Grid extension tab AND the core Brush tab
    // (via GridPlacementBrush::DrawSettingsUI). The two surfaces are
    // sibling tabs in the same tab bar — only one renders per frame,
    // so identical ImGui IDs don't collide.
    void DrawSharedSections();
}

#endif // EDITOR
