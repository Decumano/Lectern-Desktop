// Native PDF output.
//
// On Linux the webview is WebKitGTK, whose script-driven print path can't produce
// the document the user laid out: it ignores @page margins and imposes those of
// the GTK print dialog. webkit_print_operation_print() takes the paper and margins
// directly and needs no dialog, so the finished pages are loaded into a hidden
// web view and printed from there. The operation can only print a view's main
// frame, which is why this isn't simply the app's own view.

#include "pdf_export.h"

#include <stdexcept>

#ifdef LECTERN_USE_WEBKIT_PDF

#include <gtk/gtk.h>
#include <gtk/gtkunixprint.h>
#include <webkit/webkit.h>

#include <optional>

#include "util.h"

namespace lectern::pdf {

namespace {

constexpr guint cSettleMs = 500;      // lets web fonts finish after load
constexpr guint cTimeoutSeconds = 120;

struct PrintJob
{
    GMainLoop *loop = nullptr;
    GtkWidget *view = nullptr;
    GtkPrintSettings *settings = nullptr;
    GtkPageSetup *setup = nullptr;
    WebKitPrintOperation *operation = nullptr;
    guint settle_source = 0;
    std::optional<std::string> error;
    bool done = false;
};

void fFinish(PrintJob *job, std::optional<std::string> error)
{
    if (job->done)
    {
        return;
    }
    job->done = true;
    job->error = std::move(error);
    g_main_loop_quit(job->loop);
}

void fOnPrintFinished(WebKitPrintOperation *, gpointer data)
{
    fFinish(static_cast<PrintJob *>(data), std::nullopt);
}

void fOnPrintFailed(WebKitPrintOperation *, GError *error, gpointer data)
{
    fFinish(static_cast<PrintJob *>(data),
            std::string(error != nullptr ? error->message : "printing failed"));
}

gboolean fStartPrint(gpointer data)
{
    auto *job = static_cast<PrintJob *>(data);
    job->settle_source = 0;
    job->operation = webkit_print_operation_new(WEBKIT_WEB_VIEW(job->view));

    webkit_print_operation_set_print_settings(job->operation, job->settings);
    webkit_print_operation_set_page_setup(job->operation, job->setup);
    g_signal_connect(job->operation, "finished", G_CALLBACK(fOnPrintFinished), job);
    g_signal_connect(job->operation, "failed", G_CALLBACK(fOnPrintFailed), job);
    webkit_print_operation_print(job->operation);
    return G_SOURCE_REMOVE;
}

void fOnLoadChanged(WebKitWebView *, WebKitLoadEvent event, gpointer data)
{
    auto *job = static_cast<PrintJob *>(data);

    // WebKit reports FINISHED after a failed load as well.
    if (event == WEBKIT_LOAD_FINISHED && !job->done && job->operation == nullptr
        && job->settle_source == 0)
    {
        job->settle_source = g_timeout_add(cSettleMs, fStartPrint, job);
    }
}

gboolean fOnLoadFailed(WebKitWebView *, WebKitLoadEvent, gchar *, GError *error,
                       gpointer data)
{
    fFinish(static_cast<PrintJob *>(data),
            std::string(error != nullptr ? error->message : "could not load the document"));
    return TRUE;
}

gboolean fOnTimeout(gpointer data)
{
    fFinish(static_cast<PrintJob *>(data), std::string("timed out rendering the PDF"));
    return G_SOURCE_REMOVE;
}

/// The "Print to File" printer. Its name is translated, so it can't be looked up
/// by name; it is the virtual printer that takes PDF.
gboolean fFindFilePrinter(GtkPrinter *printer, gpointer data)
{
    if (gtk_printer_is_virtual(printer) && gtk_printer_accepts_pdf(printer))
    {
        *static_cast<std::string *>(data) = gtk_printer_get_name(printer);
        return TRUE;
    }
    return FALSE;
}

} // namespace

bool fSupported()
{
    return true;
}

void fRender(const std::string &html, const std::filesystem::path &out,
             double width_mm, double height_mm)
{
    std::string printer;
    gtk_enumerate_printers(fFindFilePrinter, &printer, nullptr, TRUE);
    if (printer.empty())
    {
        throw std::runtime_error("no print-to-file backend is available");
    }

    // Rendered to a temporary file first: a failed render must not leave a
    // truncated file at the path the user chose.
    gchar *tmp_dir = g_dir_make_tmp("lectern-pdf-XXXXXX", nullptr);
    if (tmp_dir == nullptr)
    {
        throw std::runtime_error("could not create a temporary directory");
    }
    const std::filesystem::path tmp_file = std::filesystem::path(tmp_dir) / "out.pdf";
    g_free(tmp_dir);

    gchar *uri = g_filename_to_uri(tmp_file.c_str(), nullptr, nullptr);

    PrintJob job;
    job.loop = g_main_loop_new(nullptr, FALSE);

    job.settings = gtk_print_settings_new();
    gtk_print_settings_set_printer(job.settings, printer.c_str());
    gtk_print_settings_set(job.settings, GTK_PRINT_SETTINGS_OUTPUT_FILE_FORMAT, "pdf");
    gtk_print_settings_set(job.settings, GTK_PRINT_SETTINGS_OUTPUT_URI, uri);
    g_free(uri);

    GtkPaperSize *paper = gtk_paper_size_new_custom("lectern", "Lectern", width_mm,
                                                    height_mm, GTK_UNIT_MM);
    job.setup = gtk_page_setup_new();
    gtk_page_setup_set_paper_size(job.setup, paper);
    gtk_page_setup_set_top_margin(job.setup, 0, GTK_UNIT_MM);
    gtk_page_setup_set_bottom_margin(job.setup, 0, GTK_UNIT_MM);
    gtk_page_setup_set_left_margin(job.setup, 0, GTK_UNIT_MM);
    gtk_page_setup_set_right_margin(job.setup, 0, GTK_UNIT_MM);
    gtk_paper_size_free(paper);

    // Never presented: the document only has to lay out, not appear.
    GtkWidget *window = gtk_window_new();
    job.view = webkit_web_view_new();
    gtk_window_set_child(GTK_WINDOW(window), job.view);

    // The pages are finished markup. Nothing in them needs to run.
    webkit_settings_set_enable_javascript(
        webkit_web_view_get_settings(WEBKIT_WEB_VIEW(job.view)), FALSE);

    g_signal_connect(job.view, "load-changed", G_CALLBACK(fOnLoadChanged), &job);
    g_signal_connect(job.view, "load-failed", G_CALLBACK(fOnLoadFailed), &job);
    const guint timeout = g_timeout_add_seconds(cTimeoutSeconds, fOnTimeout, &job);

    webkit_web_view_load_html(WEBKIT_WEB_VIEW(job.view), html.c_str(), nullptr);
    g_main_loop_run(job.loop);

    g_source_remove(timeout);
    if (job.settle_source != 0)
    {
        g_source_remove(job.settle_source);
    }
    g_signal_handlers_disconnect_by_data(job.view, &job);
    if (job.operation != nullptr)
    {
        g_signal_handlers_disconnect_by_data(job.operation, &job);
        g_object_unref(job.operation);
    }
    gtk_window_destroy(GTK_WINDOW(window));
    g_object_unref(job.settings);
    g_object_unref(job.setup);
    g_main_loop_unref(job.loop);

    std::optional<std::string> contents;
    if (!job.error)
    {
        contents = util::fReadFile(tmp_file);
    }

    std::error_code ignored;
    std::filesystem::remove_all(tmp_file.parent_path(), ignored);

    if (job.error)
    {
        throw std::runtime_error(*job.error);
    }
    if (!contents || contents->empty())
    {
        throw std::runtime_error("the PDF renderer produced nothing");
    }
    util::fWriteFileAtomic(out, *contents);
}

} // namespace lectern::pdf

#else

namespace lectern::pdf {

bool fSupported()
{
    return false;
}

void fRender(const std::string &, const std::filesystem::path &, double, double)
{
    throw std::runtime_error("native PDF export is not available in this build");
}

} // namespace lectern::pdf

#endif
