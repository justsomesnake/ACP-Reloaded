#pragma once

// Runtime objects of the overlay; created by the lifecycle thread before hooks become active.

#include <memory>
#include <string>

#include "pad_feed.h"
#include "panel.h"
#include "sync_service.h"

namespace acp::hook {

struct OverlayRuntime {
    SyncService sync;
    std::unique_ptr<ui::Panel> panel;
    ui::PadFeeder feeder;
    std::string fontData;  // read by the lifecycle thread (no file I/O in Present)
    ui::PanelEnv env;
};

OverlayRuntime* Runtime();
void SetRuntime(OverlayRuntime* rt);

}  // namespace acp::hook
