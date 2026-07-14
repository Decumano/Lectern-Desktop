// Cloud sync engine: connects the desktop app's active work folder to an
// officesuite-web server, always over HTTP - even when the server happens to
// be on the same machine, so the app's folder never has to change out from
// under the user. A background loop + push-on-write reconcile the local
// folder against the server, using a Git-style three-way merge: edits to
// different regions of a file combine automatically, edits to the same
// region are flagged as a conflict for the user to resolve.
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use similar::TextDiff;
use std::collections::{HashMap, HashSet};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::OnceLock;
use std::time::{Duration, UNIX_EPOCH};
use tauri::{AppHandle, Manager};

/// Serializes sync passes: `nudge_sync`, the background loop, `cloud_sync_now`
/// and `cloud_connect` can all fire concurrently, and two passes reconciling
/// the same folder at once would race on state.json and the base snapshots.
fn sync_lock() -> &'static tokio::sync::Mutex<()> {
    static LOCK: OnceLock<tokio::sync::Mutex<()>> = OnceLock::new();
    LOCK.get_or_init(|| tokio::sync::Mutex::new(()))
}

/// True while a background poll loop is alive, so disconnect → reconnect
/// doesn't stack a second loop on top of the first (the old loop only exits
/// on the next tick after the config disappears).
static LOOP_RUNNING: AtomicBool = AtomicBool::new(false);

const SYNC_DIR: &str = ".officesuite-sync";
const WORK_FILE_EXTENSIONS: [&str; 8] = ["mdp", "mds", "mdg", "mdn", "mdl", "mdc", "mde", "mdb"];

/// Root-level sidecars that sync alongside work files. Currently just the
/// custom-templates store, so templates follow the account across devices
/// (the server's workspace listing exposes the same set — see
/// officesuite-web workspace.rs SYNC_SIDECAR_FILES).
const SYNC_SIDECAR_FILES: [&str; 1] = ["_lktpl.json"];
const POLL_INTERVAL_SECS: u64 = 45;

// ── Config (which server/account/folder we're connected to) ──

#[derive(Serialize, Deserialize, Clone, Default)]
struct CloudConfig {
    server_url: String,
    email: String,
    api_token: String,
    root: String,
}

fn config_path(app: &AppHandle) -> Result<PathBuf, String> {
    let dir = app.path().app_config_dir().map_err(|e| e.to_string())?;
    fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    Ok(dir.join("cloud.json"))
}

fn load_config(app: &AppHandle) -> Option<CloudConfig> {
    let path = config_path(app).ok()?;
    let data = fs::read_to_string(path).ok()?;
    serde_json::from_str(&data).ok()
}

fn save_config(app: &AppHandle, cfg: &CloudConfig) -> Result<(), String> {
    let path = config_path(app)?;
    let data = serde_json::to_string_pretty(cfg).map_err(|e| e.to_string())?;
    fs::write(path, data).map_err(|e| e.to_string())
}

fn clear_config(app: &AppHandle) -> Result<(), String> {
    let path = config_path(app)?;
    if path.exists() {
        fs::remove_file(path).map_err(|e| e.to_string())?;
    }
    Ok(())
}

// ── Per-folder sync state (the "base" snapshot registry + open conflicts) ──

#[derive(Serialize, Deserialize, Default, Clone)]
struct SyncFileState {
    hash: String,
}

#[derive(Serialize, Deserialize, Default, Clone)]
struct ConflictEntry {
    reason: String,
}

#[derive(Serialize, Deserialize, Default)]
struct SyncState {
    #[serde(default)]
    last_synced_at: u64,
    #[serde(default)]
    files: HashMap<String, SyncFileState>,
    #[serde(default)]
    conflicts: HashMap<String, ConflictEntry>,
}

fn sync_dir(root: &str) -> PathBuf {
    Path::new(root).join(SYNC_DIR)
}
fn state_path(root: &str) -> PathBuf {
    sync_dir(root).join("state.json")
}
fn base_path(root: &str, rel_path: &str) -> PathBuf {
    sync_dir(root).join("base").join(rel_path)
}
fn remote_snapshot_path(root: &str, rel_path: &str) -> PathBuf {
    sync_dir(root).join("base").join(format!("{}.remote", rel_path))
}

fn load_state(root: &str) -> SyncState {
    fs::read_to_string(state_path(root))
        .ok()
        .and_then(|s| serde_json::from_str(&s).ok())
        .unwrap_or_default()
}

fn save_state(root: &str, state: &SyncState) -> Result<(), String> {
    fs::create_dir_all(sync_dir(root)).map_err(|e| e.to_string())?;
    let data = serde_json::to_string_pretty(state).map_err(|e| e.to_string())?;
    fs::write(state_path(root), data).map_err(|e| e.to_string())
}

fn write_base(root: &str, rel_path: &str, content: &str) -> Result<(), String> {
    let p = base_path(root, rel_path);
    if let Some(parent) = p.parent() {
        fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    }
    fs::write(p, content).map_err(|e| e.to_string())
}
fn read_base(root: &str, rel_path: &str) -> Option<String> {
    fs::read_to_string(base_path(root, rel_path)).ok()
}
fn remove_base(root: &str, rel_path: &str) {
    let _ = fs::remove_file(base_path(root, rel_path));
}
fn write_remote_snapshot(root: &str, rel_path: &str, content: &str) -> Result<(), String> {
    let p = remote_snapshot_path(root, rel_path);
    if let Some(parent) = p.parent() {
        fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    }
    fs::write(p, content).map_err(|e| e.to_string())
}
fn read_remote_snapshot(root: &str, rel_path: &str) -> Option<String> {
    fs::read_to_string(remote_snapshot_path(root, rel_path)).ok()
}
fn remove_remote_snapshot(root: &str, rel_path: &str) {
    let _ = fs::remove_file(remote_snapshot_path(root, rel_path));
}
fn write_local(path: &Path, content: &str) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    }
    fs::write(path, content).map_err(|e| e.to_string())
}

fn hash_bytes(bytes: &[u8]) -> String {
    let mut hasher = Sha256::new();
    hasher.update(bytes);
    format!("{:x}", hasher.finalize())
}

fn now_ms() -> u64 {
    std::time::SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

fn walk_local_files(dir: &Path, rel_prefix: &str, out: &mut HashSet<String>) {
    let Ok(read) = fs::read_dir(dir) else { return };
    for entry in read.flatten() {
        let name = entry.file_name().to_string_lossy().to_string();
        if name.starts_with('.') {
            continue;
        }
        let path = entry.path();
        let rel = if rel_prefix.is_empty() {
            name.clone()
        } else {
            format!("{}/{}", rel_prefix, name)
        };
        if path.is_dir() {
            walk_local_files(&path, &rel, out);
        } else {
            let ext = Path::new(&name)
                .extension()
                .and_then(|e| e.to_str())
                .map(|e| e.to_lowercase())
                .unwrap_or_default();
            let is_root_sidecar =
                rel_prefix.is_empty() && SYNC_SIDECAR_FILES.contains(&name.as_str());
            if WORK_FILE_EXTENSIONS.contains(&ext.as_str()) || is_root_sidecar {
                out.insert(rel);
            }
        }
    }
}

fn local_file_paths(root: &str) -> HashSet<String> {
    let mut out = HashSet::new();
    walk_local_files(Path::new(root), "", &mut out);
    out
}

// ── HTTP client against officesuite-web ──

#[derive(Deserialize)]
struct RemoteEntry {
    #[serde(rename = "relPath")]
    rel_path: String,
    #[serde(rename = "isDir")]
    is_dir: bool,
    #[serde(default)]
    hash: String,
    #[serde(default)]
    children: Vec<RemoteEntry>,
}

fn flatten_remote(entries: &[RemoteEntry], out: &mut HashMap<String, String>) {
    for e in entries {
        if e.is_dir {
            flatten_remote(&e.children, out);
        } else {
            out.insert(e.rel_path.clone(), e.hash.clone());
        }
    }
}

fn auth_header(cfg: &CloudConfig) -> String {
    format!("Bearer {}", cfg.api_token)
}

fn workspace_file_url(cfg: &CloudConfig, rel_path: &str) -> Result<reqwest::Url, String> {
    let base = format!("{}/api/workspace/file", cfg.server_url);
    let mut url = reqwest::Url::parse(&base).map_err(|e| e.to_string())?;
    url.query_pairs_mut().append_pair("path", rel_path);
    Ok(url)
}

async fn remote_list(
    client: &reqwest::Client,
    cfg: &CloudConfig,
) -> Result<HashMap<String, String>, String> {
    let url = format!("{}/api/workspace", cfg.server_url);
    let resp = client
        .get(&url)
        .header("Authorization", auth_header(cfg))
        .send()
        .await
        .map_err(|e| e.to_string())?;
    if !resp.status().is_success() {
        return Err(format!("list failed: {}", resp.status()));
    }
    let tree: Vec<RemoteEntry> = resp.json().await.map_err(|e| e.to_string())?;
    let mut flat = HashMap::new();
    flatten_remote(&tree, &mut flat);
    Ok(flat)
}

async fn remote_read(
    client: &reqwest::Client,
    cfg: &CloudConfig,
    rel_path: &str,
) -> Result<String, String> {
    let url = workspace_file_url(cfg, rel_path)?;
    let resp = client
        .get(url)
        .header("Authorization", auth_header(cfg))
        .send()
        .await
        .map_err(|e| e.to_string())?;
    if !resp.status().is_success() {
        return Err(format!("read failed: {}", resp.status()));
    }
    resp.text().await.map_err(|e| e.to_string())
}

async fn remote_write(
    client: &reqwest::Client,
    cfg: &CloudConfig,
    rel_path: &str,
    content: &str,
) -> Result<(), String> {
    let url = workspace_file_url(cfg, rel_path)?;
    let resp = client
        .put(url)
        .header("Authorization", auth_header(cfg))
        .header("Content-Type", "text/plain;charset=utf-8")
        .body(content.to_string())
        .send()
        .await
        .map_err(|e| e.to_string())?;
    if !resp.status().is_success() {
        return Err(format!("write failed: {}", resp.status()));
    }
    Ok(())
}

async fn remote_delete(
    client: &reqwest::Client,
    cfg: &CloudConfig,
    rel_path: &str,
) -> Result<(), String> {
    let base = format!("{}/api/workspace/entry", cfg.server_url);
    let mut url = reqwest::Url::parse(&base).map_err(|e| e.to_string())?;
    url.query_pairs_mut()
        .append_pair("path", rel_path)
        .append_pair("isDir", "false");
    let resp = client
        .delete(url)
        .header("Authorization", auth_header(cfg))
        .send()
        .await
        .map_err(|e| e.to_string())?;
    if !resp.status().is_success() && resp.status() != reqwest::StatusCode::NOT_FOUND {
        return Err(format!("delete failed: {}", resp.status()));
    }
    Ok(())
}

#[derive(Serialize)]
struct Credentials<'a> {
    email: &'a str,
    password: &'a str,
}

#[derive(Deserialize)]
struct UserView {
    #[serde(rename = "apiToken")]
    api_token: String,
}

async fn login(
    client: &reqwest::Client,
    server_url: &str,
    email: &str,
    password: &str,
) -> Result<String, String> {
    let url = format!("{}/api/auth/login", server_url);
    let resp = client
        .post(&url)
        .json(&Credentials { email, password })
        .send()
        .await
        .map_err(|e| e.to_string())?;
    if !resp.status().is_success() {
        return Err(format!("login failed: {}", resp.status()));
    }
    let user: UserView = resp.json().await.map_err(|e| e.to_string())?;
    Ok(user.api_token)
}

// ── Three-way merge ──

struct Hunk {
    start: usize,
    end: usize,
    lines: Vec<String>,
}

fn split_lines(s: &str) -> Vec<&str> {
    if s.is_empty() {
        Vec::new()
    } else {
        s.split_inclusive('\n').collect()
    }
}

fn hunks_from_diff(base: &[&str], other: &[&str]) -> Vec<Hunk> {
    let diff = TextDiff::from_slices(base, other);
    diff.ops()
        .iter()
        .filter(|op| op.tag() != similar::DiffTag::Equal)
        .map(|op| {
            let old = op.old_range();
            let new = op.new_range();
            Hunk {
                start: old.start,
                end: old.end,
                lines: other[new].iter().map(|s| s.to_string()).collect(),
            }
        })
        .collect()
}

enum MergeOutcome {
    Clean(String),
    Conflict,
}

/// Git-style three-way merge: hunks that touch disjoint regions of `base`
/// combine automatically; hunks that overlap are only auto-resolved if both
/// sides made the exact same edit, otherwise this returns Conflict rather
/// than guessing.
fn three_way_merge(base: &str, local: &str, remote: &str) -> MergeOutcome {
    let base_lines = split_lines(base);
    let local_lines = split_lines(local);
    let remote_lines = split_lines(remote);

    let local_hunks = hunks_from_diff(&base_lines, &local_lines);
    let remote_hunks = hunks_from_diff(&base_lines, &remote_lines);

    #[derive(Clone, Copy)]
    enum Side {
        Local,
        Remote,
    }

    let mut tagged: Vec<(Side, usize)> = local_hunks
        .iter()
        .enumerate()
        .map(|(i, _)| (Side::Local, i))
        .chain(remote_hunks.iter().enumerate().map(|(i, _)| (Side::Remote, i)))
        .collect();
    tagged.sort_by_key(|(side, i)| match side {
        Side::Local => local_hunks[*i].start,
        Side::Remote => remote_hunks[*i].start,
    });

    let mut clusters: Vec<Vec<(Side, usize)>> = Vec::new();
    let mut cur: Vec<(Side, usize)> = Vec::new();
    let mut cur_end = 0usize;
    let mut prev_range: Option<(usize, usize)> = None;
    for item in tagged {
        let (start, end) = match item.0 {
            Side::Local => (local_hunks[item.1].start, local_hunks[item.1].end),
            Side::Remote => (remote_hunks[item.1].start, remote_hunks[item.1].end),
        };
        // Pure insertions are zero-width (start == end), so two insertions at
        // the same base position never satisfy `start < cur_end`; without the
        // second check they'd land in separate clusters and both be applied —
        // duplicating the text when both sides inserted the same lines.
        let overlaps = start < cur_end || (start == end && prev_range == Some((start, end)));
        prev_range = Some((start, end));
        if !cur.is_empty() && overlaps {
            cur.push(item);
            cur_end = cur_end.max(end);
        } else {
            if !cur.is_empty() {
                clusters.push(std::mem::take(&mut cur));
            }
            cur.push(item);
            cur_end = end;
        }
    }
    if !cur.is_empty() {
        clusters.push(cur);
    }

    let mut resolved: Vec<(usize, usize, Vec<String>)> = Vec::new();
    for cluster in &clusters {
        let locals: Vec<&Hunk> = cluster
            .iter()
            .filter_map(|(s, i)| matches!(s, Side::Local).then(|| &local_hunks[*i]))
            .collect();
        let remotes: Vec<&Hunk> = cluster
            .iter()
            .filter_map(|(s, i)| matches!(s, Side::Remote).then(|| &remote_hunks[*i]))
            .collect();

        if remotes.is_empty() && locals.len() == 1 {
            let h = locals[0];
            resolved.push((h.start, h.end, h.lines.clone()));
        } else if locals.is_empty() && remotes.len() == 1 {
            let h = remotes[0];
            resolved.push((h.start, h.end, h.lines.clone()));
        } else if locals.len() == 1
            && remotes.len() == 1
            && locals[0].start == remotes[0].start
            && locals[0].end == remotes[0].end
            && locals[0].lines == remotes[0].lines
        {
            let h = locals[0];
            resolved.push((h.start, h.end, h.lines.clone()));
        } else {
            return MergeOutcome::Conflict;
        }
    }

    let mut merged = String::new();
    let mut pos = 0usize;
    for (start, end, lines) in resolved {
        merged.push_str(&base_lines[pos..start].concat());
        for l in lines {
            merged.push_str(&l);
        }
        pos = end;
    }
    merged.push_str(&base_lines[pos..].concat());
    MergeOutcome::Clean(merged)
}

// ── Reconciliation ──

async fn sync_once(cfg: &CloudConfig) -> Result<(), String> {
    let _guard = sync_lock().lock().await;
    let client = reqwest::Client::new();
    let mut state = load_state(&cfg.root);

    let remote_hashes = remote_list(&client, cfg).await?;
    let local_paths = local_file_paths(&cfg.root);

    let mut all_paths: HashSet<String> = local_paths;
    all_paths.extend(remote_hashes.keys().cloned());
    all_paths.extend(state.files.keys().cloned());

    for rel_path in all_paths {
        if state.conflicts.contains_key(&rel_path) {
            // Leave conflicted files alone until the user resolves them.
            continue;
        }

        let local_full = Path::new(&cfg.root).join(&rel_path);
        let local_content = if local_full.is_file() {
            fs::read_to_string(&local_full).ok()
        } else {
            None
        };
        let local_hash = local_content.as_ref().map(|c| hash_bytes(c.as_bytes()));
        let remote_hash = remote_hashes.get(&rel_path).cloned();
        let base_hash = state.files.get(&rel_path).map(|f| f.hash.clone());

        let local_changed = local_hash != base_hash;
        let remote_changed = remote_hash != base_hash;

        match (local_changed, remote_changed) {
            (false, false) => {}
            (true, false) => {
                if let Some(content) = &local_content {
                    remote_write(&client, cfg, &rel_path, content).await?;
                    write_base(&cfg.root, &rel_path, content)?;
                    state
                        .files
                        .insert(rel_path.clone(), SyncFileState { hash: local_hash.clone().unwrap() });
                } else if base_hash.is_some() {
                    remote_delete(&client, cfg, &rel_path).await?;
                    remove_base(&cfg.root, &rel_path);
                    state.files.remove(&rel_path);
                }
            }
            (false, true) => {
                if let Some(hash) = &remote_hash {
                    let content = remote_read(&client, cfg, &rel_path).await?;
                    write_local(&local_full, &content)?;
                    write_base(&cfg.root, &rel_path, &content)?;
                    state
                        .files
                        .insert(rel_path.clone(), SyncFileState { hash: hash.clone() });
                } else if base_hash.is_some() {
                    let _ = fs::remove_file(&local_full);
                    remove_base(&cfg.root, &rel_path);
                    state.files.remove(&rel_path);
                }
            }
            (true, true) => match (&local_content, &remote_hash) {
                (None, None) => {
                    remove_base(&cfg.root, &rel_path);
                    state.files.remove(&rel_path);
                }
                (Some(local_text), Some(_)) => {
                    if base_hash.is_some() {
                        let remote_text = remote_read(&client, cfg, &rel_path).await?;
                        let base_text = read_base(&cfg.root, &rel_path).unwrap_or_default();
                        match three_way_merge(&base_text, local_text, &remote_text) {
                            MergeOutcome::Clean(merged) => {
                                write_local(&local_full, &merged)?;
                                remote_write(&client, cfg, &rel_path, &merged).await?;
                                write_base(&cfg.root, &rel_path, &merged)?;
                                state.files.insert(
                                    rel_path.clone(),
                                    SyncFileState { hash: hash_bytes(merged.as_bytes()) },
                                );
                            }
                            MergeOutcome::Conflict => {
                                write_remote_snapshot(&cfg.root, &rel_path, &remote_text)?;
                                state
                                    .conflicts
                                    .insert(rel_path.clone(), ConflictEntry { reason: "edit-edit".into() });
                            }
                        }
                    } else if local_hash == remote_hash {
                        write_base(&cfg.root, &rel_path, local_text)?;
                        state
                            .files
                            .insert(rel_path.clone(), SyncFileState { hash: local_hash.clone().unwrap() });
                    } else {
                        let remote_text = remote_read(&client, cfg, &rel_path).await?;
                        write_remote_snapshot(&cfg.root, &rel_path, &remote_text)?;
                        state
                            .conflicts
                            .insert(rel_path.clone(), ConflictEntry { reason: "created-both".into() });
                    }
                }
                _ => {
                    let remote_text = if remote_hash.is_some() {
                        remote_read(&client, cfg, &rel_path).await.unwrap_or_default()
                    } else {
                        String::new()
                    };
                    write_remote_snapshot(&cfg.root, &rel_path, &remote_text)?;
                    state
                        .conflicts
                        .insert(rel_path.clone(), ConflictEntry { reason: "edit-delete".into() });
                }
            },
        }
    }

    state.last_synced_at = now_ms();
    save_state(&cfg.root, &state)?;
    Ok(())
}

fn insert_suffix(rel_path: &str, suffix: &str) -> String {
    match rel_path.rsplit_once('.') {
        Some((base, ext)) => format!("{}{}.{}", base, suffix, ext),
        None => format!("{}{}", rel_path, suffix),
    }
}

async fn resolve_conflict_impl(cfg: &CloudConfig, rel_path: String, choice: String) -> Result<(), String> {
    let _guard = sync_lock().lock().await;
    let mut state = load_state(&cfg.root);
    if !state.conflicts.contains_key(&rel_path) {
        return Err("no such conflict".to_string());
    }

    let client = reqwest::Client::new();
    let local_full = Path::new(&cfg.root).join(&rel_path);
    let remote_text = read_remote_snapshot(&cfg.root, &rel_path).unwrap_or_default();
    let local_text = fs::read_to_string(&local_full).unwrap_or_default();

    match choice.as_str() {
        "local" => {
            remote_write(&client, cfg, &rel_path, &local_text).await?;
            write_base(&cfg.root, &rel_path, &local_text)?;
            state
                .files
                .insert(rel_path.clone(), SyncFileState { hash: hash_bytes(local_text.as_bytes()) });
        }
        "remote" => {
            write_local(&local_full, &remote_text)?;
            write_base(&cfg.root, &rel_path, &remote_text)?;
            state
                .files
                .insert(rel_path.clone(), SyncFileState { hash: hash_bytes(remote_text.as_bytes()) });
        }
        "both" => {
            let copy_rel = insert_suffix(&rel_path, " (remote copy)");
            let copy_full = Path::new(&cfg.root).join(&copy_rel);
            write_local(&copy_full, &remote_text)?;
            remote_write(&client, cfg, &copy_rel, &remote_text).await?;
            write_base(&cfg.root, &copy_rel, &remote_text)?;
            state
                .files
                .insert(copy_rel, SyncFileState { hash: hash_bytes(remote_text.as_bytes()) });

            remote_write(&client, cfg, &rel_path, &local_text).await?;
            write_base(&cfg.root, &rel_path, &local_text)?;
            state
                .files
                .insert(rel_path.clone(), SyncFileState { hash: hash_bytes(local_text.as_bytes()) });
        }
        _ => return Err("invalid choice".to_string()),
    }

    state.conflicts.remove(&rel_path);
    remove_remote_snapshot(&cfg.root, &rel_path);
    save_state(&cfg.root, &state)?;
    Ok(())
}

// ── Background loop + immediate push-on-write hook ──

fn start_background_loop(app: AppHandle) {
    if LOOP_RUNNING.swap(true, Ordering::SeqCst) {
        // A loop from a previous connection is still alive; it re-reads the
        // config every tick, so it will pick up the new connection on its own.
        return;
    }
    tauri::async_runtime::spawn(async move {
        loop {
            tokio::time::sleep(Duration::from_secs(POLL_INTERVAL_SECS)).await;
            match load_config(&app) {
                Some(cfg) => {
                    let _ = sync_once(&cfg).await;
                }
                None => break,
            }
        }
        LOOP_RUNNING.store(false, Ordering::SeqCst);
    });
}

/// Fired after a local write/move/delete/create succeeds, so pushes feel
/// close to instant instead of waiting for the next poll tick. Cheap no-op
/// when cloud sync isn't configured.
pub fn nudge_sync(app: &AppHandle) {
    if let Some(cfg) = load_config(app) {
        tauri::async_runtime::spawn(async move {
            let _ = sync_once(&cfg).await;
        });
    }
}

/// Called once from `run()`'s setup hook so a connection resumes syncing
/// after the app restarts without the user reconnecting.
pub fn resume_on_startup(app: &AppHandle) {
    if load_config(app).is_some() {
        start_background_loop(app.clone());
    }
}

// ── Commands exposed to the frontend ──

#[derive(Serialize, Clone)]
pub struct CloudStatus {
    connected: bool,
    #[serde(rename = "serverUrl")]
    server_url: String,
    email: String,
    #[serde(rename = "lastSyncedAt")]
    last_synced_at: u64,
    #[serde(rename = "conflictCount")]
    conflict_count: usize,
}

fn status_from_config(cfg: &CloudConfig) -> CloudStatus {
    let state = load_state(&cfg.root);
    CloudStatus {
        connected: true,
        server_url: cfg.server_url.clone(),
        email: cfg.email.clone(),
        last_synced_at: state.last_synced_at,
        conflict_count: state.conflicts.len(),
    }
}

fn disconnected_status() -> CloudStatus {
    CloudStatus {
        connected: false,
        server_url: String::new(),
        email: String::new(),
        last_synced_at: 0,
        conflict_count: 0,
    }
}

#[tauri::command]
pub async fn cloud_connect(
    app: AppHandle,
    server_url: String,
    email: String,
    password: String,
    root: String,
) -> Result<CloudStatus, String> {
    let server_url = server_url.trim_end_matches('/').to_string();
    let client = reqwest::Client::new();
    let api_token = login(&client, &server_url, &email, &password).await?;

    let cfg = CloudConfig {
        server_url,
        email,
        api_token,
        root,
    };

    save_config(&app, &cfg)?;
    // Make connect atomic: if the first sync fails (server unreachable,
    // token rejected, ...), roll the config back so the app doesn't sit in a
    // half-connected state with no background loop running.
    if let Err(e) = sync_once(&cfg).await {
        let _ = clear_config(&app);
        return Err(e);
    }
    start_background_loop(app);
    Ok(status_from_config(&cfg))
}

#[tauri::command]
pub fn cloud_disconnect(app: AppHandle) -> Result<(), String> {
    clear_config(&app)
}

#[tauri::command]
pub fn cloud_status(app: AppHandle) -> CloudStatus {
    load_config(&app).map(|cfg| status_from_config(&cfg)).unwrap_or_else(disconnected_status)
}

#[tauri::command]
pub async fn cloud_sync_now(app: AppHandle) -> Result<CloudStatus, String> {
    let cfg = load_config(&app).ok_or("not connected")?;
    sync_once(&cfg).await?;
    Ok(status_from_config(&cfg))
}

#[derive(Serialize)]
pub struct ConflictView {
    #[serde(rename = "relPath")]
    rel_path: String,
    #[serde(rename = "localContent")]
    local_content: String,
    #[serde(rename = "remoteContent")]
    remote_content: String,
}

#[tauri::command]
pub fn cloud_list_conflicts(app: AppHandle) -> Result<Vec<ConflictView>, String> {
    let cfg = load_config(&app).ok_or("not connected")?;
    let state = load_state(&cfg.root);
    let mut out = Vec::new();
    for rel_path in state.conflicts.keys() {
        let local_full = Path::new(&cfg.root).join(rel_path);
        let local_content = fs::read_to_string(&local_full).unwrap_or_default();
        let remote_content = read_remote_snapshot(&cfg.root, rel_path).unwrap_or_default();
        out.push(ConflictView {
            rel_path: rel_path.clone(),
            local_content,
            remote_content,
        });
    }
    Ok(out)
}

#[tauri::command]
pub async fn cloud_resolve_conflict(app: AppHandle, rel_path: String, choice: String) -> Result<(), String> {
    let cfg = load_config(&app).ok_or("not connected")?;
    resolve_conflict_impl(&cfg, rel_path, choice).await
}

#[cfg(test)]
mod tests {
    use super::*;

    fn clean(base: &str, local: &str, remote: &str) -> String {
        match three_way_merge(base, local, remote) {
            MergeOutcome::Clean(s) => s,
            MergeOutcome::Conflict => panic!("expected a clean merge, got a conflict"),
        }
    }

    fn conflicts(base: &str, local: &str, remote: &str) -> bool {
        matches!(three_way_merge(base, local, remote), MergeOutcome::Conflict)
    }

    #[test]
    fn disjoint_edits_merge_cleanly() {
        let base = "one\ntwo\nthree\nfour\nfive\n";
        let local = "one\nTWO\nthree\nfour\nfive\n";
        let remote = "one\ntwo\nthree\nfour\nFIVE\n";
        assert_eq!(clean(base, local, remote), "one\nTWO\nthree\nfour\nFIVE\n");
    }

    #[test]
    fn identical_edit_on_both_sides_is_not_a_conflict() {
        let base = "one\ntwo\nthree\n";
        let local = "one\nTWO\nthree\n";
        let remote = "one\nTWO\nthree\n";
        assert_eq!(clean(base, local, remote), "one\nTWO\nthree\n");
    }

    #[test]
    fn overlapping_edits_conflict() {
        let base = "one\ntwo\nthree\n";
        let local = "one\nTWO-LOCAL\nthree\n";
        let remote = "one\nTWO-REMOTE\nthree\n";
        assert!(conflicts(base, local, remote));
    }

    #[test]
    fn insert_only_on_one_side_merges_cleanly() {
        let base = "one\ntwo\nthree\n";
        let local = "one\ntwo\nthree\n";
        let remote = "one\ntwo\ntwo-and-a-half\nthree\n";
        assert_eq!(clean(base, local, remote), "one\ntwo\ntwo-and-a-half\nthree\n");
    }

    #[test]
    fn identical_insertion_on_both_sides_is_not_duplicated() {
        let base = "one\ntwo\n";
        let local = "one\ntwo\nthree\n";
        let remote = "one\ntwo\nthree\n";
        assert_eq!(clean(base, local, remote), "one\ntwo\nthree\n");
    }

    #[test]
    fn differing_insertions_at_same_position_conflict() {
        let base = "one\ntwo\n";
        let local = "one\ntwo\nLOCAL\n";
        let remote = "one\ntwo\nREMOTE\n";
        assert!(conflicts(base, local, remote));
    }

    #[test]
    fn identical_mid_file_insertions_are_not_duplicated() {
        let base = "one\nthree\n";
        let local = "one\ntwo\nthree\n";
        let remote = "one\ntwo\nthree\n";
        assert_eq!(clean(base, local, remote), "one\ntwo\nthree\n");
    }

    /// Live end-to-end check against a running officesuite-web server.
    /// Ignored by default; run with:
    ///   OFFICESUITE_TEST_SERVER=http://localhost:8080 \
    ///   OFFICESUITE_TEST_EMAIL=sync-test@example.com \
    ///   OFFICESUITE_TEST_PASSWORD=some-password \
    ///   cargo test --lib live_sync_round_trip -- --ignored
    /// Registers the account if it doesn't exist yet.
    #[tokio::test]
    #[ignore]
    async fn live_sync_round_trip() {
        let server = std::env::var("OFFICESUITE_TEST_SERVER").expect("OFFICESUITE_TEST_SERVER");
        let email = std::env::var("OFFICESUITE_TEST_EMAIL").expect("OFFICESUITE_TEST_EMAIL");
        let password = std::env::var("OFFICESUITE_TEST_PASSWORD").expect("OFFICESUITE_TEST_PASSWORD");

        let client = reqwest::Client::new();
        let api_token = match login(&client, &server, &email, &password).await {
            Ok(token) => token,
            Err(_) => {
                let resp = client
                    .post(format!("{}/api/auth/register", server))
                    .json(&Credentials { email: &email, password: &password })
                    .send()
                    .await
                    .expect("register request");
                assert!(resp.status().is_success(), "register failed: {}", resp.status());
                let user: UserView = resp.json().await.expect("register response");
                user.api_token
            }
        };

        let root_dir =
            std::env::temp_dir().join(format!("officesuite-sync-test-{}", uuid::Uuid::new_v4()));
        fs::create_dir_all(&root_dir).unwrap();
        let cfg = CloudConfig {
            server_url: server,
            email,
            api_token,
            root: root_dir.to_string_lossy().to_string(),
        };

        // 1. Local create → pushed to the server.
        let rel = format!("sync-test-{}.mdp", uuid::Uuid::new_v4());
        fs::write(root_dir.join(&rel), "# hello\nline\n").unwrap();
        sync_once(&cfg).await.expect("push sync");
        assert_eq!(
            remote_read(&client, &cfg, &rel).await.expect("remote read"),
            "# hello\nline\n"
        );

        // 2. Remote edit → pulled down.
        remote_write(&client, &cfg, &rel, "# hello\nline\nremote line\n")
            .await
            .unwrap();
        sync_once(&cfg).await.expect("pull sync");
        assert_eq!(
            fs::read_to_string(root_dir.join(&rel)).unwrap(),
            "# hello\nline\nremote line\n"
        );

        // 3. Concurrent disjoint edits → clean three-way merge on both sides.
        fs::write(root_dir.join(&rel), "# HELLO\nline\nremote line\n").unwrap();
        remote_write(&client, &cfg, &rel, "# hello\nline\nREMOTE LINE\n")
            .await
            .unwrap();
        sync_once(&cfg).await.expect("merge sync");
        let merged = "# HELLO\nline\nREMOTE LINE\n";
        assert_eq!(fs::read_to_string(root_dir.join(&rel)).unwrap(), merged);
        assert_eq!(remote_read(&client, &cfg, &rel).await.unwrap(), merged);

        // 4. Local delete → deleted on the server.
        fs::remove_file(root_dir.join(&rel)).unwrap();
        sync_once(&cfg).await.expect("delete sync");
        assert!(remote_read(&client, &cfg, &rel).await.is_err());

        let _ = fs::remove_dir_all(&root_dir);
    }

    /// The custom-templates sidecar (_lktpl.json) must sync in both
    /// directions even though it isn't a work-extension file. Same setup as
    /// live_sync_round_trip; run with the same env vars, plus --ignored.
    #[tokio::test]
    #[ignore]
    async fn live_sync_templates_sidecar() {
        let server = std::env::var("OFFICESUITE_TEST_SERVER").expect("OFFICESUITE_TEST_SERVER");
        let email = std::env::var("OFFICESUITE_TEST_EMAIL").expect("OFFICESUITE_TEST_EMAIL");
        let password = std::env::var("OFFICESUITE_TEST_PASSWORD").expect("OFFICESUITE_TEST_PASSWORD");

        let client = reqwest::Client::new();
        let api_token = login(&client, &server, &email, &password).await.expect("login");

        let root_dir =
            std::env::temp_dir().join(format!("officesuite-tpl-test-{}", uuid::Uuid::new_v4()));
        fs::create_dir_all(&root_dir).unwrap();
        let cfg = CloudConfig {
            server_url: server,
            email,
            api_token,
            root: root_dir.to_string_lossy().to_string(),
        };

        // Remember the server-side store so the account's real templates can
        // be restored afterwards (the sidecar is shared account state).
        let original = remote_read(&client, &cfg, "_lktpl.json").await.ok();

        // 1. Local template store → pushed to the server.
        let local_v1 = "{\n  \"From Desktop\": \"# desktop template\\n\"\n}";
        fs::write(root_dir.join("_lktpl.json"), local_v1).unwrap();
        sync_once(&cfg).await.expect("push sync");
        assert_eq!(
            remote_read(&client, &cfg, "_lktpl.json").await.expect("remote read"),
            local_v1
        );

        // 2. Server-side change (e.g. saved from the web app) → pulled down.
        let remote_v2 = "{\n  \"From Web\": \"# web template\\n\"\n}";
        remote_write(&client, &cfg, "_lktpl.json", remote_v2).await.unwrap();
        sync_once(&cfg).await.expect("pull sync");
        assert_eq!(
            fs::read_to_string(root_dir.join("_lktpl.json")).unwrap(),
            remote_v2
        );

        // Restore whatever the account had before the test.
        match original {
            Some(content) => remote_write(&client, &cfg, "_lktpl.json", &content).await.unwrap(),
            None => remote_delete(&client, &cfg, "_lktpl.json").await.unwrap(),
        }
        let _ = fs::remove_dir_all(&root_dir);
    }
}
