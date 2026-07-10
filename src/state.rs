use sqlx::SqlitePool;
use std::path::PathBuf;

#[derive(Clone)]
pub struct AppState {
    pub db: SqlitePool,
    pub workspaces_dir: PathBuf,
}
