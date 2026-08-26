// GTK 4 implementations of the Linux dialogs, included by dialogs.cpp when
// LECTERN_USE_GTK_DIALOGS is defined.
//
// This is not a new dependency: on Linux saucer draws into WebKitGTK, which
// links GTK 4 into the process already. Using it directly rather than shelling
// out to zenity matters for two reasons.
//
// Inside a Flatpak sandbox there is no zenity — it is an application, not part
// of org.gnome.Platform — so the shell-out path leaves the app unable to pick
// a work folder at all. GtkFileDialog, by contrast, routes through
// xdg-desktop-portal automatically when it detects a sandbox, which is both
// the only thing that works there and the more correct behaviour outside it:
// the portal grants access to exactly the folder the user chose, instead of
// relying on `--filesystem=home` blanket access.
//
// The GTK 4 dialog API is asynchronous, so each call spins a nested GMainLoop
// and quits it from the completion callback. That is the standard way to make
// these modal, and it is safe here because the exposed functions that call
// them run on the UI thread (launch::sync).
#include <gtk/gtk.h>

#include <memory>

namespace {

/// Shared state between a dialog call and its completion callback.
struct GtkDialogWait
{
    GMainLoop *loop = nullptr;
    std::optional<fs::path> path;
    bool confirmed = false;
};

void on_select_folder(GObject *source, GAsyncResult *result, gpointer data)
{
    auto *wait = static_cast<GtkDialogWait *>(data);
    GError *error = nullptr;
    GFile *file = gtk_file_dialog_select_folder_finish(
        GTK_FILE_DIALOG(source), result, &error);

    if (file != nullptr)
    {
        char *raw = g_file_get_path(file);
        if (raw != nullptr)
        {
            wait->path = fs::path(raw);
            g_free(raw);
        }
        g_object_unref(file);
    }
    // A cancelled dialog reports an error; that is not a failure worth
    // surfacing, it is the user saying no.
    g_clear_error(&error);
    g_main_loop_quit(wait->loop);
}

void on_save(GObject *source, GAsyncResult *result, gpointer data)
{
    auto *wait = static_cast<GtkDialogWait *>(data);
    GError *error = nullptr;
    GFile *file =
        gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &error);

    if (file != nullptr)
    {
        char *raw = g_file_get_path(file);
        if (raw != nullptr)
        {
            wait->path = fs::path(raw);
            g_free(raw);
        }
        g_object_unref(file);
    }
    g_clear_error(&error);
    g_main_loop_quit(wait->loop);
}

void on_alert(GObject *source, GAsyncResult *result, gpointer data)
{
    auto *wait = static_cast<GtkDialogWait *>(data);
    GError *error = nullptr;
    const int choice = gtk_alert_dialog_choose_finish(
        GTK_ALERT_DIALOG(source), result, &error);

    // Button 1 is the affirmative one; dismissing the dialog reports an error
    // and counts as "no".
    wait->confirmed = (error == nullptr) && (choice == 1);
    g_clear_error(&error);
    g_main_loop_quit(wait->loop);
}

/// Runs a nested main loop until the callback quits it.
void pump(GtkDialogWait &wait)
{
    wait.loop = g_main_loop_new(nullptr, FALSE);
    g_main_loop_run(wait.loop);
    g_main_loop_unref(wait.loop);
    wait.loop = nullptr;
}

std::optional<fs::path> gtk_pick_folder(const std::string &title)
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, title.c_str());
    gtk_file_dialog_set_modal(dialog, TRUE);

    GtkDialogWait wait;
    gtk_file_dialog_select_folder(dialog, nullptr, nullptr, on_select_folder,
                                  &wait);
    pump(wait);

    g_object_unref(dialog);
    return wait.path;
}

std::optional<fs::path> gtk_save_file(const std::string &title,
                                      const std::string &suggested_name)
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, title.c_str());
    gtk_file_dialog_set_modal(dialog, TRUE);
    if (!suggested_name.empty())
    {
        gtk_file_dialog_set_initial_name(dialog, suggested_name.c_str());
    }

    GtkDialogWait wait;
    gtk_file_dialog_save(dialog, nullptr, nullptr, on_save, &wait);
    pump(wait);

    g_object_unref(dialog);
    return wait.path;
}

bool gtk_confirm(const std::string &title, const std::string &message)
{
    GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", title.c_str());
    gtk_alert_dialog_set_detail(dialog, message.c_str());
    gtk_alert_dialog_set_modal(dialog, TRUE);

    const char *buttons[] = {"Not now", "Install", nullptr};
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_set_cancel_button(dialog, 0);
    gtk_alert_dialog_set_default_button(dialog, 1);

    GtkDialogWait wait;
    gtk_alert_dialog_choose(dialog, nullptr, nullptr, on_alert, &wait);
    pump(wait);

    g_object_unref(dialog);
    return wait.confirmed;
}

}  // namespace
