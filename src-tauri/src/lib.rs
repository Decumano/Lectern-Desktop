// Learn more about Tauri commands at https://tauri.app/develop/calling-rust/
use serde::Serialize;
use std::fs;
use std::path::Path;
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
                "**Spreadsheets** formula-capable grid stored as CSV in markdown\n",
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

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_dialog::init())
        .invoke_handler(tauri::generate_handler![greet, defaultFile, save_file])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
