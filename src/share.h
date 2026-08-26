// Sharing and comments. An owner can share a file or folder from their
// workspace with another registered account at one of three permission
// levels (view < comment < edit); grantees reach the shared content through
// the /api/shared/* endpoints, which resolve a share id (never a raw owner
// path) and enforce the granted level server-side. Comments are stored
// against the file's owner + path so everyone with access sees one thread,
// and are only allowed on work files.
#pragma once

namespace lectern::share {

void register_routes();

}  // namespace lectern::share
