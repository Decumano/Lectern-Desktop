// Learn more about Tauri commands at https://tauri.app/develop/calling-rust/
mod cloud;

use serde::Serialize;
use std::fs;
use std::path::Path;
use std::time::UNIX_EPOCH;
use tauri::AppHandle;
use tauri_plugin_dialog::DialogExt;

#[derive(Serialize)]
struct MyData {
    pub name: String,
    pub docType: String,
    pub content: String,
}

#[tauri::command]
fn greet(name: &str) -> String {
    format!("Hello, {}! You've been greeted from Rust!", name)
}

#[tauri::command]
#[warn(non_snake_case)]
fn defaultFile(name: &str) -> Result<MyData, String> {
    Ok
    (
        MyData
        {
            name: format!("Welcome to Lore Keep {}", name),
            docType: "doc".to_string(),
            content: concat!
            (
                "# Welcome to Lore Keep\n\n",
                "Lore Keep is a lightweight office suite that stores everything in **Markdown**.\n\n",
                "## Features\n\n",
                "**Documents** rich markdown editing with live preview\n",
                "**Spreadsheets** formula-capable grid stored as cell=value pairs\n",
                "## Markdown Quick Reference\n",
                "| Element | Syntax |\n",
                "|---------|--------|\n",
                "| Bold | **text** |\n",
                "| Italic | *text* |\n",
                "| Heading | # H1 ## H2 |\n",
                "| List | - item |\n",
                "| Blockquote | > text |\n",
                "| Code | ``` code ``` |\n",
                "| Link | [text](url) |\n\n",
                "## Getting Started\n\n",
                "1. Click **New** in the sidebar to create a file\n",
                "2. Write in Markdown, use the toolbar for formatting\n",
                "3. Toggle the **split** view button to preview alongside your writing\n",
                "4. Click the **download** button to export your file\n\n",
                "> All files are saved automatically to your browser\'s local storage.\n\n",
                "Happy writing!"
            ).to_string()
        }
    )
}

#[tauri::command]
fn save_file(app: AppHandle, name: String, content: String) -> Result<bool, String> {
    let extension = Path::new(&name)
        .extension()
        .and_then(|ext| ext.to_str())
        .unwrap_or("txt")
        .to_string();

    let file_path = app
        .dialog()
        .file()
        .set_file_name(&name)
        .add_filter("File", &[extension.as_str()])
        .blocking_save_file();

    let path = match file_path {
        Some(path) => path,
        None => return Ok(false),
    };

    let path_buf = path.into_path().map_err(|e| e.to_string())?;

    fs::write(path_buf, content).map_err(|e| e.to_string())?;

    Ok(true)
}

// ── WORK FOLDER (on-disk storage for Docs/Sheets/Graphs) ──

#[derive(Serialize)]
struct FsEntry {
    name: String,
    #[serde(rename = "relPath")]
    rel_path: String,
    #[serde(rename = "isDir")]
    is_dir: bool,
    modified: u64,
    children: Vec<FsEntry>,
}

const WORK_FILE_EXTENSIONS: [&str; 8] = ["mdp", "mds", "mdg", "mdn", "mdl", "mdc", "mde", "mdb"];

fn modified_ms(metadata: &fs::Metadata) -> u64 {
    metadata
        .modified()
        .ok()
        .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

fn walk_work_dir(dir: &Path, rel_prefix: &str) -> Result<Vec<FsEntry>, String> {
    let mut entries = Vec::new();

    for entry in fs::read_dir(dir).map_err(|e| e.to_string())? {
        let entry = entry.map_err(|e| e.to_string())?;
        let path = entry.path();
        let name = entry.file_name().to_string_lossy().to_string();

        // Skip dot-prefixed entries (e.g. `.officesuite-sync/`, the cloud sync
        // engine's metadata folder) so internal bookkeeping never shows up in
        // the file tree.
        if name.starts_with('.') {
            continue;
        }

        let metadata = entry.metadata().map_err(|e| e.to_string())?;

        let rel_path = if rel_prefix.is_empty() {
            name.clone()
        } else {
            format!("{}/{}", rel_prefix, name)
        };

        if metadata.is_dir() {
            entries.push(FsEntry {
                name,
                children: walk_work_dir(&path, &rel_path)?,
                rel_path,
                is_dir: true,
                modified: 0,
            });
        } else {
            let ext = Path::new(&name)
                .extension()
                .and_then(|e| e.to_str())
                .map(|e| e.to_lowercase())
                .unwrap_or_default();

            if !WORK_FILE_EXTENSIONS.contains(&ext.as_str()) {
                continue;
            }

            entries.push(FsEntry {
                name,
                rel_path,
                is_dir: false,
                modified: modified_ms(&metadata),
                children: Vec::new(),
            });
        }
    }

    entries.sort_by(|a, b| match (a.is_dir, b.is_dir) {
        (true, false) => std::cmp::Ordering::Less,
        (false, true) => std::cmp::Ordering::Greater,
        _ => a.name.to_lowercase().cmp(&b.name.to_lowercase()),
    });

    Ok(entries)
}

#[tauri::command]
fn pick_work_folder(app: AppHandle) -> Option<String> {
    app.dialog()
        .file()
        .blocking_pick_folder()
        .and_then(|p| p.into_path().ok())
        .map(|p| p.to_string_lossy().to_string())
}

#[tauri::command]
fn list_work_folder(root: String) -> Result<Vec<FsEntry>, String> {
    walk_work_dir(Path::new(&root), "")
}

#[tauri::command]
fn read_work_file(root: String, rel_path: String) -> Result<String, String> {
    fs::read_to_string(Path::new(&root).join(&rel_path)).map_err(|e| e.to_string())
}

#[tauri::command]
fn write_work_file(app: AppHandle, root: String, rel_path: String, content: String) -> Result<(), String> {
    let path = Path::new(&root).join(&rel_path);

    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    }

    fs::write(path, content).map_err(|e| e.to_string())?;
    cloud::nudge_sync(&app);
    Ok(())
}

#[tauri::command]
fn create_work_folder(app: AppHandle, root: String, rel_path: String) -> Result<(), String> {
    fs::create_dir_all(Path::new(&root).join(&rel_path)).map_err(|e| e.to_string())?;
    cloud::nudge_sync(&app);
    Ok(())
}

#[tauri::command]
fn delete_work_entry(app: AppHandle, root: String, rel_path: String, is_dir: bool) -> Result<(), String> {
    let path = Path::new(&root).join(&rel_path);

    if is_dir {
        fs::remove_dir_all(path).map_err(|e| e.to_string())?;
    } else {
        fs::remove_file(path).map_err(|e| e.to_string())?;
    }
    cloud::nudge_sync(&app);
    Ok(())
}

#[tauri::command]
fn move_work_entry(
    app: AppHandle,
    root: String,
    from_rel_path: String,
    to_rel_path: String,
) -> Result<(), String> {
    let from = Path::new(&root).join(&from_rel_path);
    let to = Path::new(&root).join(&to_rel_path);

    if let Some(parent) = to.parent() {
        fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    }

    fs::rename(from, to).map_err(|e| e.to_string())?;
    cloud::nudge_sync(&app);
    Ok(())
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_dialog::init())
        .setup(|app| {
            cloud::resume_on_startup(app.handle());
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            greet,
            defaultFile,
            save_file,
            pick_work_folder,
            list_work_folder,
            read_work_file,
            write_work_file,
            create_work_folder,
            delete_work_entry,
            move_work_entry,
            cloud::cloud_connect,
            cloud::cloud_disconnect,
            cloud::cloud_status,
            cloud::cloud_sync_now,
            cloud::cloud_list_conflicts,
            cloud::cloud_resolve_conflict
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
