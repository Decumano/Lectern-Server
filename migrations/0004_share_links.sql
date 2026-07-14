-- "Anyone with the link" sharing: the row id doubles as the unguessable URL
-- token (UUIDv4), which is the whole capability — no account required to use
-- one. Permission levels mirror account shares (view / comment / edit).

CREATE TABLE share_links (
    id         TEXT PRIMARY KEY,
    owner_id   TEXT NOT NULL REFERENCES users(id),
    rel_path   TEXT NOT NULL,
    is_dir     INTEGER NOT NULL,
    permission TEXT NOT NULL CHECK (permission IN ('view', 'comment', 'edit')),
    created_at INTEGER NOT NULL
);

CREATE INDEX idx_share_links_owner ON share_links(owner_id, rel_path);

-- Link visitors may not have an account, so comments need a nullable author
-- (rendered as "Anonymous"). SQLite can't relax NOT NULL in place; rebuild.
ALTER TABLE comments RENAME TO comments_v1;

CREATE TABLE comments (
    id         TEXT PRIMARY KEY,
    owner_id   TEXT NOT NULL REFERENCES users(id),
    rel_path   TEXT NOT NULL,
    author_id  TEXT REFERENCES users(id),
    body       TEXT NOT NULL,
    created_at INTEGER NOT NULL
);

INSERT INTO comments SELECT * FROM comments_v1;
DROP TABLE comments_v1;

CREATE INDEX idx_comments_file ON comments(owner_id, rel_path);
