// WebSocket endpoint /api/sync/ws and the hook the write handlers call.
// See sync_hub.h for the protocol.
#pragma once

#include <string>
#include <vector>

namespace lectern::sync {

void fRegisterRoutes();

/// Called after a successful change to `owner_id`'s workspace so every other
/// client of that account (and anyone following the file) hears about it.
/// `origin_device` is the writer's X-Lectern-Device header, possibly empty.
/// Never throws: a notification problem must not fail the write that caused it.
void fNotifyChanges(const std::string &owner_id,
                    const std::vector<std::string> &paths,
                    const std::string &origin_device);

}  // namespace lectern::sync
