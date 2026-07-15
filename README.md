# OfficeSuite Web

A self-hosted, multi-user web port of [OfficeSuite](https://github.com/Decumano/OfficeSuite), a
lightweight Markdown-based office suite originally built as a Tauri desktop app. This version
replaces the Tauri/Rust desktop backend with a Rust web server (axum) and adds account-based auth
so each user gets their own isolated workspace, since there's no OS folder-picker on the web.

The frontend (`web/`) is forked from
[officesuite-frontend](https://github.com/Decumano/officesuite-frontend) — the UI and editor logic
(`main.js`) are unchanged; only `platform.js` was extended to talk to this server's HTTP API
instead of Tauri's IPC, and a small login/register gate was added in front of the app.

## Running locally

```
cp .env.example .env    # adjust SESSION_SECRET etc.
cargo run
```

The server listens on `http://localhost:8080` by default, serves the static frontend from `web/`,
and exposes the API under `/api`. SQLite data and per-user workspaces are written to `data/`
(gitignored) unless overridden via `DATABASE_URL` / `WORKSPACES_DIR`.

## Running with Docker

```
docker build -t officesuite-web .
docker run -p 8080:8080 -v officesuite-data:/data officesuite-web
```

### Running with docker-compose

```
cp .env.example .env    # set SESSION_SECRET at minimum
docker compose up -d --build
```

`docker-compose.yml` reads its settings from `.env` (via `${VAR}` substitution) so the container
gets the same configuration as `cargo run` would:

| Variable         | Default                            | Notes                                      |
|-------------------|-------------------------------------|---------------------------------------------|
| `SESSION_SECRET`  | *(required, no default)*            | Signs session cookies; at least 64 chars. Compose refuses to start without this set |
| `COOKIE_SECURE`   | `false`                             | Set `true` once behind HTTPS so session cookies carry the Secure flag |
| `HOST_PORT`       | `8080`                              | Host-side port mapping                      |
| `PORT`            | `8080`                              | Port the server listens on, both on the host and inside the container |
| `TAG`             | `latest`                            | Tag applied to the built image              |

`DATABASE_URL` and `WORKSPACES_DIR` aren't read from `.env` for the compose service — their
image defaults (set in the `Dockerfile`) already point at `/data`, which is backed by the
`officesuite-data` named volume, so data persists across `docker compose down`/`up` cycles.
The `.env` values for those two are only used by local `cargo run`.

## API

| Method | Path                  | Description                              |
|--------|------------------------|-------------------------------------------|
| POST   | `/api/auth/register`   | Create an account, starts a session       |
| POST   | `/api/auth/login`      | Log in, starts a session                  |
| POST   | `/api/auth/logout`     | Ends the session                          |
| GET    | `/api/auth/me`         | Current user, or 401                      |
| GET    | `/api/workspace`       | List the current user's files/folders     |
| GET    | `/api/workspace/file`  | Read a file (`?path=`)                    |
| PUT    | `/api/workspace/file`  | Write a file (`?path=`, raw body)         |
| POST   | `/api/workspace/folder`| Create a folder (`{ relPath }`)           |
| DELETE | `/api/workspace/entry` | Delete a file/folder (`?path=&isDir=`)    |
| POST   | `/api/workspace/move`  | Move/rename (`{ from, to }`)              |
| POST   | `/api/shares`          | Share a file/folder with another account (`{ relPath, isDir, email, permission }`, permission: `view`/`comment`/`edit`) |
| GET    | `/api/shares`          | List who an entry is shared with (`?path=`) |
| DELETE | `/api/shares/:id`      | Revoke a share (owner) or leave it (grantee) |
| GET    | `/api/shared`          | Entries shared with the current user      |
| GET    | `/api/shared/list`     | File tree of a folder share (`?share=`)   |
| GET    | `/api/shared/file`     | Read a shared file (`?share=&path=`)      |
| PUT    | `/api/shared/file`     | Write a shared file (edit permission; existing files only) |
| POST   | `/api/links`           | Create a share link (`{ relPath, isDir, permission }`); returns the token |
| GET    | `/api/links`           | List an entry's share links (`?path=`)    |
| DELETE | `/api/links/:id`       | Revoke a share link (owner only)          |
| GET    | `/api/link/:token`     | Link metadata — **no login needed**, the token is the authorization |
| GET    | `/api/link/:token/list`| File tree of a folder link                |
| GET/PUT| `/api/link/:token/file`| Read / write through a link (`?path=`; write needs an edit link) |
| GET    | `/api/comments`        | Comments on a work file (`?path=` own, `?share=&subPath=` shared, or `?link=&subPath=` via link — the link form needs no login) |
| GET    | `/api/fonts`           | List the current account's custom fonts   |
| POST   | `/api/fonts`           | Upload a font (`?familyName=&filename=`, raw body; .ttf/.otf/.woff/.woff2, 5MB max, 30 fonts max) |
| GET    | `/api/fonts/:id`       | Raw font bytes (owner only)               |
| DELETE | `/api/fonts/:id`       | Delete a font                             |
| POST   | `/api/comments`        | Add a comment (comment/edit permission; any work file; optional `anchor` JSON pins it to a text range / cell range / item) |
| DELETE | `/api/comments/:id`    | Delete a comment (author or file owner)   |

Every workspace endpoint scopes reads/writes to `data/workspaces/<user_id>/`, derived from the
session — never from client input — and rejects any relative path that tries to escape that
directory (no `..`, no absolute paths).

## Environment variables

See `.env.example`: `DATABASE_URL`, `SESSION_SECRET`, `PORT`, `WORKSPACES_DIR`, `COOKIE_SECURE`.

## Desktop app downloads

The login gate links to `/download.html`, which lists the installers and portable
builds attached to the latest GitHub Release of the repo named by
`DESKTOP_RELEASES_REPO` (default [Decumano/OfficeSuite](https://github.com/Decumano/OfficeSuite/releases)).
Releases are produced by that repo's `release.yml` workflow — push a `v*` tag there
and the downloads show up here automatically; nothing on this server needs redeploying.

The page asks this server first (`/api/downloads/latest`), which lists the release
and streams the assets itself — required while the releases repo is **private**,
since browsers can't reach it directly. Set `GITHUB_RELEASES_TOKEN` to a fine-grained
PAT with *Contents: Read-only* on that repo (the token stays server-side). If the
repo is public, the token is unnecessary and the page can also fall back to calling
the GitHub API from the browser.

## Custom fonts

Settings &rarr; Custom fonts lets each account upload font files (TTF/OTF/WOFF/WOFF2) that then render
the same way for everyone who opens that account's documents — in the live editor/preview and in
exported PDFs — regardless of what's installed on the visitor's machine or this server's container.

This is deliberately separate from the font *picker*'s `queryLocalFonts()` path (which lists fonts
already on the visitor's own device): that browser API only exists in a secure context (HTTPS, or
literally `localhost`) and only in Chromium, so it silently contributes nothing on a plain-HTTP
deployment. Uploaded fonts don't have that limitation — they're served as ordinary HTTP assets, so
`@font-face` works in any browser over any origin. PDF export (headless Chromium, sandboxed to only
`file://`/`data:` requests — see `src/export.rs`) gets the account's fonts base64-embedded directly
into the exported HTML, since that sandbox can't fetch them any other way.

Installing fonts into the server's own OS/container (e.g. via `fonts/` + the Dockerfile) is a
separate, optional mechanism that only affects PDF export's system-font fallback — it does **not**
make a font available in the live editor, since that runs entirely in each visitor's own browser.

## Security notes

- `SESSION_SECRET` (≥ 64 chars) signs session cookies; without it the server runs with
  unsigned cookies and logs a warning (fine for local dev only).
- Login failures and registrations are rate-limited per client IP (10 failed logins / 15 min,
  20 registrations / hour). Successful logins are never counted, so many users behind one
  proxy IP won't lock each other out.
- Desktop clients authenticate with `Authorization: Bearer <apiToken>` (returned by
  login/register); browsers use the session cookie.
