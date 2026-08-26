#include "dialogs.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

#include "util.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlwapi.h>
#endif

namespace fs = std::filesystem;

namespace lectern::dialogs {

#ifdef LECTERN_USE_GTK_DIALOGS
#include "dialogs_gtk.inl"
#endif

namespace {

#ifdef _WIN32

std::wstring widen(const std::string &value)
{
    if (value.empty())
    {
        return {};
    }
    const int size = MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8,
                        0,
                        value.data(),
                        static_cast<int>(value.size()),
                        out.data(),
                        size);
    return out;
}

/// RAII for the per-thread COM apartment the shell dialogs need. The webview
/// already initialises COM on the UI thread, so a second call returning
/// RPC_E_CHANGED_MODE is expected and must not be treated as failure — it just
/// means we must not uninitialise it either.
class ComScope
{
  public:
    ComScope()
        : owned_(SUCCEEDED(
              CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
    {
    }

    ~ComScope()
    {
        if (owned_)
        {
            CoUninitialize();
        }
    }

    ComScope(const ComScope &) = delete;
    ComScope &operator=(const ComScope &) = delete;

  private:
    bool owned_;
};

std::optional<fs::path> run_dialog(const std::string &title,
                                   bool folders,
                                   const std::string &suggested_name,
                                   const std::string &extension)
{
    const ComScope com;

    IFileDialog *dialog = nullptr;
    const CLSID clsid = folders ? CLSID_FileOpenDialog : CLSID_FileSaveDialog;
    const IID iid = folders ? IID_IFileOpenDialog : IID_IFileSaveDialog;

    if (FAILED(CoCreateInstance(
            clsid, nullptr, CLSCTX_INPROC_SERVER, iid,
            reinterpret_cast<void **>(&dialog))))
    {
        return std::nullopt;
    }

    struct Release
    {
        IFileDialog *dialog;
        ~Release()
        {
            dialog->Release();
        }
    } release{dialog};

    if (!title.empty())
    {
        dialog->SetTitle(widen(title).c_str());
    }

    if (folders)
    {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST);
    }
    else
    {
        if (!suggested_name.empty())
        {
            dialog->SetFileName(widen(suggested_name).c_str());
        }
        if (!extension.empty())
        {
            const std::wstring wide_ext = widen(extension);
            const std::wstring spec = L"*." + wide_ext;
            const COMDLG_FILTERSPEC filter[] = {{L"File", spec.c_str()},
                                                {L"All files", L"*.*"}};
            dialog->SetFileTypes(2, filter);
            dialog->SetDefaultExtension(wide_ext.c_str());
        }
    }

    if (dialog->Show(nullptr) != S_OK)
    {
        return std::nullopt;  // cancelled
    }

    IShellItem *item = nullptr;
    if (FAILED(dialog->GetResult(&item)) || item == nullptr)
    {
        return std::nullopt;
    }

    PWSTR raw = nullptr;
    const bool ok = SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw));
    fs::path result;
    if (ok && raw != nullptr)
    {
        result = fs::path(raw);
        CoTaskMemFree(raw);
    }
    item->Release();

    if (result.empty())
    {
        return std::nullopt;
    }
    return result;
}

#else

/// Runs a command and returns its trimmed stdout, or nullopt when the command
/// is missing or exits non-zero (which is also how a cancelled dialog reports
/// itself).
std::optional<std::string> capture(const std::string &command)
{
    FILE *pipe = popen(command.c_str(), "r");
    if (pipe == nullptr)
    {
        return std::nullopt;
    }

    std::string out;
    std::array<char, 4096> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) !=
           nullptr)
    {
        out += buffer.data();
    }

    if (pclose(pipe) != 0)
    {
        return std::nullopt;
    }

    const std::string trimmed = util::trim(out);
    if (trimmed.empty())
    {
        return std::nullopt;
    }
    return trimmed;
}

bool has_command(const std::string &name)
{
    return std::system(("command -v " + name + " > /dev/null 2>&1").c_str()) ==
           0;
}

/// Single-quotes a value for the shell, so a title or filename containing a
/// quote can't break out into a command.
std::string shell_quote(const std::string &value)
{
    std::string out = "'";
    for (const char c : value)
    {
        if (c == '\'')
        {
            out += "'\\''";
        }
        else
        {
            out.push_back(c);
        }
    }
    out += "'";
    return out;
}

#endif

}  // namespace

std::optional<fs::path> pick_folder(const std::string &title)
{
#ifdef _WIN32
    return run_dialog(title, /*folders=*/true, {}, {});
#elif defined(__APPLE__)
    const auto chosen = capture(
        "osascript -e 'POSIX path of (choose folder with prompt " +
        shell_quote(title) + ")' 2>/dev/null");
    return chosen ? std::optional(fs::path(*chosen)) : std::nullopt;
#else
#ifdef LECTERN_USE_GTK_DIALOGS
    // GTK routes through xdg-desktop-portal inside a Flatpak sandbox, where
    // no external dialog helper exists.
    return gtk_pick_folder(title);
#else
    if (has_command("zenity"))
    {
        const auto chosen =
            capture("zenity --file-selection --directory --title=" +
                    shell_quote(title) + " 2>/dev/null");
        return chosen ? std::optional(fs::path(*chosen)) : std::nullopt;
    }
    if (has_command("kdialog"))
    {
        const auto chosen = capture("kdialog --getexistingdirectory . --title " +
                                    shell_quote(title) + " 2>/dev/null");
        return chosen ? std::optional(fs::path(*chosen)) : std::nullopt;
    }
    throw std::runtime_error(
        "no folder dialog available — install zenity or kdialog");
#endif
#endif
}

std::optional<fs::path> save_file(const std::string &title,
                                  const std::string &suggested_name,
                                  const std::string &extension)
{
#ifdef _WIN32
    return run_dialog(title, /*folders=*/false, suggested_name, extension);
#elif defined(__APPLE__)
    const auto chosen = capture(
        "osascript -e 'POSIX path of (choose file name with prompt " +
        shell_quote(title) + " default name " + shell_quote(suggested_name) +
        ")' 2>/dev/null");
    return chosen ? std::optional(fs::path(*chosen)) : std::nullopt;
#else
    (void)extension;
#ifdef LECTERN_USE_GTK_DIALOGS
    return gtk_save_file(title, suggested_name);
#else
    if (has_command("zenity"))
    {
        const auto chosen =
            capture("zenity --file-selection --save --confirm-overwrite"
                    " --title=" +
                    shell_quote(title) + " --filename=" +
                    shell_quote(suggested_name) + " 2>/dev/null");
        return chosen ? std::optional(fs::path(*chosen)) : std::nullopt;
    }
    if (has_command("kdialog"))
    {
        const auto chosen = capture("kdialog --getsavefilename " +
                                    shell_quote(suggested_name) + " --title " +
                                    shell_quote(title) + " 2>/dev/null");
        return chosen ? std::optional(fs::path(*chosen)) : std::nullopt;
    }
    throw std::runtime_error(
        "no save dialog available — install zenity or kdialog");
#endif
#endif
}


bool confirm(const std::string &title, const std::string &message)
{
#ifdef _WIN32
    return MessageBoxW(nullptr,
                       widen(message).c_str(),
                       widen(title).c_str(),
                       MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) == IDYES;
#elif defined(__APPLE__)
    // `display dialog` exits non-zero when the user cancels, which capture()
    // already reports as nullopt.
    return capture("osascript -e 'display dialog " + shell_quote(message) +
                   " with title " + shell_quote(title) +
                   " buttons {\"Not now\", \"Install\"} default button 2' "
                   "2>/dev/null")
        .has_value();
#else
#ifdef LECTERN_USE_GTK_DIALOGS
    return gtk_confirm(title, message);
#else
    if (has_command("zenity"))
    {
        return std::system(("zenity --question --title=" + shell_quote(title) +
                            " --text=" + shell_quote(message) +
                            " > /dev/null 2>&1")
                               .c_str()) == 0;
    }
    if (has_command("kdialog"))
    {
        return std::system(("kdialog --yesno " + shell_quote(message) +
                            " --title " + shell_quote(title) +
                            " > /dev/null 2>&1")
                               .c_str()) == 0;
    }
    return false;  // no way to ask; treat as "no"
#endif
#endif
}

void reveal(const fs::path &path)
{
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", widen(path.string()).c_str(), nullptr,
                  nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    (void)std::system(("open " + shell_quote(path.string()) +
                       " > /dev/null 2>&1")
                          .c_str());
#else
    (void)std::system(("xdg-open " + shell_quote(path.string()) +
                       " > /dev/null 2>&1")
                          .c_str());
#endif
}

}  // namespace lectern::dialogs
