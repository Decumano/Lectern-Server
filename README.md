# Lectern Server (C++)

A C++ port of [Lectern-Server](../Lectern-Server) — the self-hosted, multi-user
web version of Lectern. Same API, same database, same frontend; the Rust/axum
backend is replaced by C++/Drogon.

The frontend (`web/`) is untouched: it is the same `officesuite-frontend`
submodule the Rust build and the desktop app use, and no file in it was
modified for this port.

## Building

Needs a C++20 compiler, CMake ≥ 3.25 and vcpkg.

```bash
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
```

```bash
cmake --build build --config Release
```

On Windows the Visual Studio generator with MSVC is what this was developed
against:

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=E:/vcpkg/scripts/buildsystems/vcpkg.cmake
```

## Running

```bash
cp .env.example .env
```

Then run the binary from a directory containing `web/` and `migrations/`, or
point `WEB_DIR` and `MIGRATIONS_DIR` at them. The server listens on
`http://localhost:8080`, serves the frontend, and exposes the API under `/api`.
SQLite data and per-user workspaces are written to `data/`.

### The frontend

The server serves `web/`, the same `officesuite-frontend` submodule the desktop
app uses. In a clone that has it registered:

```bash
git submodule update --init --recursive
```

**This directory is not a git repository yet**, so that command has nothing to
act on and the `.gitmodules` file here stays inert until it is one. Either
initialise it:

```bash
git init && git submodule add https://github.com/Decumano/officesuite-frontend.git web
```

…or skip the submodule and point the server at a checkout you already have —
the frontend is identical in every copy:

```bash
WEB_DIR=../Lectern-Server/web ./build/Release/lectern-server
```

Docker works the same way as the Rust build — `docker compose up -d --build`,
with `SESSION_SECRET` set in `.env`.

## Tests

```bash
./build/Release/lectern-tests
```

## What changed, and why

The API surface, status codes, JSON field names, rate limits, quotas and path
validation are all ports rather than redesigns. Five things did have to change.

**The database layer is synchronous.** sqlx's async pool has no C++ equivalent
worth the complexity here: Drogon's ORM needs the framework rebuilt with its
`orm`+`sqlite3` features, and every handler in this application already blocks
on filesystem I/O anyway. `src/db.cpp` wraps the SQLite C API behind one mutex,
and `setThreadNum` gives Drogon a pool to absorb the blocking. SQLite is a local
file; a query is microseconds, not a network round trip.

**Migrations are applied at runtime.** sqlx's `migrate!` macro embeds them at
compile time. Here `Db::run_migrations` reads `migrations/*.sql` in filename
order at startup and records what it applied in a `_migrations` table. The seven
existing migration files are byte-identical to the Rust build's.

**Sessions moved into a table this codebase owns.** Drogon's built-in sessions
live in process memory, which would sign every user out on restart. `0008_sessions.sql`
adds a `sessions` table and `src/session.cpp` implements the same contract
tower-sessions provided: HMAC-signed cookies, 30-day sliding expiry,
`SameSite=Strict`, `Secure` behind `COOKIE_SECURE`.

**Existing password hashes still work.** libsodium's `crypto_pwhash_str_verify`
parses the Argon2id parameters out of the stored PHC string, so accounts created
by the Rust server log in unchanged. New hashes use libsodium's interactive
parameters rather than the `argon2` crate's defaults.

**Release-asset downloads are buffered, not streamed.** The Rust build piped
GitHub's response body straight through. Drogon's async streams are driven from
the event loop while cpr's write callback runs on a worker thread, and bridging
the two safely needs a hand-written handshake for no real gain — desktop
installers are single-digit megabytes. `DOWNLOAD_MAX_BYTES` (default 256 MB)
bounds it explicitly.

### PDF export

`/api/export/pdf` still drives a headless Chromium over the DevTools Protocol,
and still locks the tab down the same way: JS execution disabled outright, and a
`Fetch`-domain interceptor that allowlists only `file://` and `data:` so the
visitor-authored export HTML cannot be used for SSRF. There is no C++ equivalent
of the `headless_chrome` crate, so `src/export.cpp` speaks CDP directly over a
WebSocket (Drogon's `WebSocketClient` on a dedicated event loop). That was worth
doing rather than falling back to `chrome --print-to-pdf`, which would lose both
the script lockdown and the request allowlist.

**On Windows this needs Google Chrome, not Edge.** Edge's DevTools port binds
and reports as `LISTENING`, but Windows' AppContainer isolation refuses loopback
connections to it from another process — every attach fails after a stuck
`SYN_RECEIVED`. `find_browser` deliberately does not list Edge so the failure is
"no browser found" rather than a confusing timeout. Set `CHROME_PATH` if Chrome
is somewhere non-standard.

This is the one endpoint that could not be exercised end-to-end during the port:
the development machine had only Edge installed. Everything else below was.

## Verified

Against a live build, with the frontend submodule from the Rust checkout:

- All 8 migrations apply to a fresh database
- Register, login, `/me`, logout, and session invalidation after logout
- Bearer-token auth (the desktop app's path) accepted; a bad token rejected
- Workspace write / list / read / stat / move / delete
- Path traversal (`../../etc/passwd`) rejected as `invalid path`
- Deletion tombstones recorded on delete and on move, cleared on rewrite
- Share links: create, anonymous metadata, anonymous read, anonymous write
  refused at `view`/`comment` level
- Anonymous comments through a comment link, visible to the owner
- Font upload rejecting bad extensions and CSS-injection family names
- `Cache-Control: no-cache` on HTML, `max-age=3600` on assets, nothing on `/api`
- SPA fallback serving `index.html` at 200 for unmatched routes
- 58 unit assertions (email validation, base64 vectors, path validation)
