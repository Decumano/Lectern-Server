-- File/folder sharing between accounts, plus per-document comments.

CREATE TABLE shares (
    id         TEXT PRIMARY KEY,
    owner_id   TEXT NOT NULL REFERENCES users(id),
    grantee_id TEXT NOT NULL REFERENCES users(id),
    -- Path inside the owner's workspace; identifies the shared file or folder.
    rel_path   TEXT NOT NULL,
    is_dir     INTEGER NOT NULL,
    permission TEXT NOT NULL CHECK (permission IN ('view', 'comment', 'edit')),
    created_at INTEGER NOT NULL,
    UNIQUE (owner_id, grantee_id, rel_path)
);

CREATE INDEX idx_shares_grantee ON shares(grantee_id);

CREATE TABLE comments (
    id         TEXT PRIMARY KEY,
    -- Whose workspace the commented file lives in (comments follow the file's
    -- owner, so every sharer sees the same thread).
    owner_id   TEXT NOT NULL REFERENCES users(id),
    rel_path   TEXT NOT NULL,
    author_id  TEXT NOT NULL REFERENCES users(id),
    body       TEXT NOT NULL,
    created_at INTEGER NOT NULL
);

CREATE INDEX idx_comments_file ON comments(owner_id, rel_path);
