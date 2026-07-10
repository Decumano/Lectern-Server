# web/ — frontend (forked from officesuite-frontend)

Static frontend (HTML/CSS/JS, no build step), forked from
[officesuite-frontend](https://github.com/Decumano/officesuite-frontend) (originally built for
[OfficeSuite](https://github.com/Decumano/OfficeSuite), a Tauri desktop app) and adapted here for
web deployment. See the top-level [README.md](../README.md) for how this fits into
`officesuite-web` as a whole.

## Structure

- `index.html`, `folio-office-suite.html` — app shell / markup
- `main.js` — application logic (unchanged from upstream — it only ever calls `Platform.*`)
- `styles.css` — styling
- `platform.js` — the backend adapter (see below); extended here with a web-API branch
- `auth-gate.js` — new: login/register screen shown before `main.js` loads on the web build
- `vendor/` — third-party libraries (see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md))
- `assets/` — static assets

## Platform adapter

`platform.js` isolates every backend call behind a single `Platform` object (`pickWorkFolder`,
`readWorkFile`, `writeWorkFile`, `listWorkFolder`, `createWorkFolder`, `deleteWorkEntry`,
`moveWorkEntry`, `defaultFile`, `saveFile`, plus `login`/`register`/`logout`/`currentUser` added for
this fork). `main.js` never talks to a backend directly.

- **On Tauri**, `platform.js` detects `window.__TAURI__` and delegates to `invoke(...)` calls, same
  as upstream.
- **Here (web, no Tauri)**, the six work-folder methods call this repo's `/api/workspace/*`
  endpoints (see `../src/workspace.rs`) instead of rejecting as unavailable. The server derives the
  workspace root from the logged-in session, so `root`/`pickWorkFolder()` is just a sentinel string
  once authenticated — there's no real folder to pick on the web.
