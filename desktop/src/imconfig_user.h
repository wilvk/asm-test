// imconfig_user.h — injected into EVERY imgui + addon TU via -DIMGUI_USER_CONFIG
// (mk/desktop.mk), so the digest-pinned imgui tarball is never edited
// (docs/internal/gui/13-foundation-moves.md F2). imgui.cpp does
// `#ifdef IMGUI_USER_CONFIG #include IMGUI_USER_CONFIG #endif`; -Idesktop/src
// (already in DESKTOP_CXXFLAGS) resolves this file.
#pragma once

// 32-bit draw indices. The binding constraint is the HEADLESS NULL-BACKEND
// golden-test tier: it has no renderer backend, so it never sets
// RendererHasVtxOffset, and 16-bit indices there cap any single dense draw at
// 64k vertices — a wall for ImPlot heatmaps, node canvases, and the app's own
// draw-list views. NOT "OpenGL 3.0": the live app is usually fine (macOS
// requests a 3.2 core context in main.cpp; Linux compat contexts resolve to
// 4.x). Widening is a strict superset — existing draws are unaffected.
#define ImDrawIdx unsigned int
