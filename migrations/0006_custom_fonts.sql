-- Account-level custom fonts: uploaded once, available in every document the
-- user opens (live editor via @font-face, PDF export via a base64-embedded
-- @font-face — see src/fonts.rs) regardless of what's installed on the
-- visitor's machine or the server's container.
CREATE TABLE custom_fonts (
    id            TEXT PRIMARY KEY,
    owner_id      TEXT NOT NULL REFERENCES users(id),
    family_name   TEXT NOT NULL,
    filename      TEXT NOT NULL,
    content_type  TEXT NOT NULL,
    byte_size     INTEGER NOT NULL,
    created_at    INTEGER NOT NULL,
    UNIQUE (owner_id, family_name)
);

CREATE INDEX idx_custom_fonts_owner ON custom_fonts(owner_id);
