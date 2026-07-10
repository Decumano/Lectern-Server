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
| `SESSION_SECRET`  | *(required, no default)*            | Compose refuses to start without this set   |
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

Every workspace endpoint scopes reads/writes to `data/workspaces/<user_id>/`, derived from the
session — never from client input — and rejects any relative path that tries to escape that
directory (no `..`, no absolute paths).

## Environment variables

See `.env.example`: `DATABASE_URL`, `SESSION_SECRET`, `PORT`, `WORKSPACES_DIR`.
