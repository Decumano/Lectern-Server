use axum::http::HeaderMap;
use sqlx::SqlitePool;
use std::collections::{HashMap, HashSet};
use std::net::{IpAddr, SocketAddr};
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use crate::downloads::Releases;

#[derive(Clone)]
pub struct AppState {
    pub db: SqlitePool,
    pub workspaces_dir: PathBuf,
    pub fonts_dir: PathBuf,
    pub auth_limiter: RateLimiter,
    pub releases: Arc<Releases>,
    /// Peer addresses (the reverse proxy in front of this server) whose
    /// X-Forwarded-For header is trusted for rate-limit bucketing. Parsed
    /// once from TRUSTED_PROXIES at startup.
    pub trusted_proxies: Arc<HashSet<IpAddr>>,
    /// Per-account workspace size ceiling in bytes; 0 disables the check.
    pub workspace_quota_bytes: u64,
}

impl AppState {
    /// The IP the rate limiter should bucket a request under. Directly
    /// exposed servers use the TCP peer address. Behind a reverse proxy every
    /// request arrives from the proxy's IP — one shared bucket, so a single
    /// abuser could exhaust `register`/`anon-comment` for everyone. When the
    /// peer is listed in TRUSTED_PROXIES, use the rightmost X-Forwarded-For
    /// entry instead: that's the value our own proxy appended, the entries
    /// left of it are client-controlled and stay untrusted.
    pub fn client_ip(&self, addr: &SocketAddr, headers: &HeaderMap) -> IpAddr {
        let peer = addr.ip();
        if !self.trusted_proxies.contains(&peer) {
            return peer;
        }
        headers
            .get("x-forwarded-for")
            .and_then(|v| v.to_str().ok())
            .and_then(|v| v.rsplit(',').next())
            .and_then(|v| v.trim().parse::<IpAddr>().ok())
            .unwrap_or(peer)
    }
}

/// Longest window any caller uses; entries older than this are dropped on the
/// next hit so the map can't grow without bound.
const MAX_WINDOW: Duration = Duration::from_secs(60 * 60);

/// In-memory fixed-window counters keyed by (client IP, bucket name), used to
/// throttle auth abuse: password brute force on login and mass account
/// creation on register. Behind a reverse proxy every client shares the
/// proxy's IP, so login only counts *failed* attempts — something legitimate
/// users rarely rack up — rather than all traffic.
#[derive(Clone, Default)]
pub struct RateLimiter {
    buckets: Arc<Mutex<HashMap<(IpAddr, &'static str), (u32, Instant)>>>,
}

impl RateLimiter {
    /// True when the bucket already reached `max` hits inside `window`.
    pub fn is_limited(&self, ip: IpAddr, kind: &'static str, max: u32, window: Duration) -> bool {
        let buckets = self.buckets.lock().unwrap();
        match buckets.get(&(ip, kind)) {
            Some((count, start)) if start.elapsed() < window => *count >= max,
            _ => false,
        }
    }

    /// Records one hit against the bucket, starting a fresh window if the
    /// previous one expired.
    pub fn record(&self, ip: IpAddr, kind: &'static str, window: Duration) {
        let mut buckets = self.buckets.lock().unwrap();
        buckets.retain(|_, (_, start)| start.elapsed() < MAX_WINDOW);
        let entry = buckets.entry((ip, kind)).or_insert((0, Instant::now()));
        if entry.1.elapsed() >= window {
            *entry = (0, Instant::now());
        }
        entry.0 += 1;
    }
}
