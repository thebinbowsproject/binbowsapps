// Internet Exploader — GTK4 + WebKitGTK port
//
// Layout: a top toolbar (Start / Back / Forward / Reload / search box /
// Search button) above a GtkPaned split view. The left pane shows a
// scrollable list of every .html file found in "sites" (each row shows
// the page's <title> and a fake "www.[filename].com" address). The right
// pane is a WebKitWebView that actually renders the selected page.
//
// Behavior matches the original WinForms version:
//   - Search always re-lists every .html file in sites/, regardless of
//     what's typed in the search box (search does not filter).
//   - Clicking a result hides the list and shows the page.
//   - Back/Forward/Reload work via two history stacks, same as before.
//   - Start button (top-left) and the startup landing page both jump to
//     LaunchAssets/IELAUNCHPAGE.html.

#include <gtk/gtk.h>
#include <webkit/webkit.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ===================== App state =====================
// GTK4's callback model is C-style (plain function pointers + a single
// user_data pointer), so all the mutable state that used to live as
// fields on Form1 lives here instead, passed around via one pointer.
struct AppState {
    GtkWidget *window = nullptr;

    GtkWidget *start_btn = nullptr;
    GtkWidget *back_btn = nullptr;
    GtkWidget *forward_btn = nullptr;
    GtkWidget *reload_btn = nullptr;
    GtkWidget *search_entry = nullptr;
    GtkWidget *search_btn = nullptr;

    GtkWidget *paned = nullptr;          // left/right split
    GtkWidget *results_scroller = nullptr;
    GtkWidget *results_box = nullptr;    // vertical box of result rows
    GtkWidget *webview = nullptr;        // WebKitWebView, cast to GtkWidget*

    std::string sites_folder;
    std::string launch_page;

    std::vector<std::string> back_history;
    std::vector<std::string> forward_history;
    std::string current_page;            // empty == no page loaded yet
    bool has_current_page = false;
};

// ===================== Forward declarations =====================
static void load_all_files(AppState *app);
static void open_page(AppState *app, const std::string &file);
static void navigate_browser(AppState *app, const std::string &file_path);
static void update_nav_buttons(AppState *app);
static void show_results_panel(AppState *app, bool show);
static void go_to_start_page(AppState *app);

// ===================== Small helpers =====================

// Reads a whole file into a string. Returns empty string on failure.
static std::string read_file(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Strips a path + extension down to just the filename without extension,
// e.g. "/a/b/My Page 2.html" -> "My Page 2"
static std::string filename_without_ext(const std::string &path) {
    fs::path p(path);
    return p.stem().string();
}

// Pulls the text between <title> and </title> out of an html file.
// Falls back to the filename (without extension) if there's no title
// tag or the file can't be read.
static std::string get_page_title(const std::string &file) {
    std::string html = read_file(file);

    if (!html.empty()) {
        // Case-insensitive, dot-matches-newline equivalent: build the
        // regex with icase, and use [\s\S] instead of '.' to span lines.
        static const std::regex title_re(
            R"(<title[^>]*>([\s\S]*?)</title>)",
            std::regex::icase);

        std::smatch m;
        if (std::regex_search(html, m, title_re)) {
            std::string title = m[1].str();

            // trim whitespace
            auto not_space = [](unsigned char c) { return !std::isspace(c); };
            auto start = std::find_if(title.begin(), title.end(), not_space);
            auto end = std::find_if(title.rbegin(), title.rend(), not_space).base();
            if (start < end) {
                title = std::string(start, end);
                if (!title.empty()) return title;
            }
        }
    }

    return filename_without_ext(file);
}

// Turns "My Cool Site 2.html" into "mycoolsite2" for a fake
// www.mycoolsite2.com address. Strips anything that isn't a-z/0-9.
static std::string get_fake_domain_slug(const std::string &file) {
    std::string name = filename_without_ext(file);
    std::transform(name.begin(), name.end(), name.begin(),
                    [](unsigned char c) { return std::tolower(c); });

    std::string slug;
    for (char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            slug += c;
        }
    }
    return slug.empty() ? "site" : slug;
}

// Converts a plain filesystem path into a file:// URI WebKit can load.
static std::string to_file_uri(const std::string &path) {
    GFile *gfile = g_file_new_for_path(path.c_str());
    char *uri = g_file_get_uri(gfile);
    std::string result = uri ? uri : "";
    g_free(uri);
    g_object_unref(gfile);
    return result;
}

// ===================== Results list =====================

// Removes every child currently in results_box (equivalent to the C#
// version's resultsPanel.Controls.Clear() + Dispose()). GTK4 widgets are
// reference counted, so gtk_box_remove() handles cleanup correctly.
static void clear_results_box(AppState *app) {
    GtkWidget *child = gtk_widget_get_first_child(app->results_box);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(app->results_box), child);
        child = next;
    }
}

struct ResultClickData {
    AppState *app;
    std::string file;
};

static void on_result_clicked(GtkGestureClick * /*gesture*/, int /*n_press*/,
                               double /*x*/, double /*y*/, gpointer user_data) {
    auto *data = static_cast<ResultClickData *>(user_data);
    open_page(data->app, data->file);
}

static void on_result_row_destroy(gpointer user_data) {
    delete static_cast<ResultClickData *>(user_data);
}

// Builds one row: a clickable box containing the page title (in blue,
// underlined, like a link) and a green fake "www.name.com" address
// underneath it — same look as the WinForms version, minus the literal
// file path which we deliberately removed per an earlier request.
static GtkWidget *build_result_row(AppState *app, const std::string &file) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_top(row, 6);
    gtk_widget_set_margin_bottom(row, 6);
    gtk_widget_set_margin_start(row, 8);
    gtk_widget_set_margin_end(row, 8);

    std::string title_text = get_page_title(file);
    std::string url_text = "www." + get_fake_domain_slug(file) + ".com";

    GtkWidget *title_label = gtk_label_new(nullptr);
    {
        // Pango markup for blue + underline, similar to the WinForms look.
        std::string safe;
        for (char c : title_text) {
            if (c == '&') safe += "&amp;";
            else if (c == '<') safe += "&lt;";
            else if (c == '>') safe += "&gt;";
            else safe += c;
        }
        std::string markup = "<span foreground='#0000FF' underline='single'>" + safe + "</span>";
        gtk_label_set_markup(GTK_LABEL(title_label), markup.c_str());
    }
    gtk_widget_set_halign(title_label, GTK_ALIGN_START);

    GtkWidget *url_label = gtk_label_new(nullptr);
    {
        std::string markup = "<span foreground='#008000' size='small'>" + url_text + "</span>";
        gtk_label_set_markup(GTK_LABEL(url_label), markup.c_str());
    }
    gtk_widget_set_halign(url_label, GTK_ALIGN_START);

    gtk_box_append(GTK_BOX(row), title_label);
    gtk_box_append(GTK_BOX(row), url_label);

    // Make the whole row clickable, like the C# version's panel.Click
    // wired on the panel + both labels.
    GtkGesture *click = gtk_gesture_click_new();
    auto *data = new ResultClickData{app, file};
    g_signal_connect(click, "released", G_CALLBACK(on_result_clicked), data);
    g_object_set_data_full(G_OBJECT(row), "click-data", data, on_result_row_destroy);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(click));

    // Visual hint that the row is clickable.
    gtk_widget_set_cursor_from_name(row, "pointer");

    return row;
}

// Lists every .html file found in app->sites_folder. This is also what
// the Search button calls — search always shows everything, regardless
// of what's typed, matching the explicit earlier request.
static void load_all_files(AppState *app) {
    clear_results_box(app);

    std::error_code ec;
    if (!fs::exists(app->sites_folder, ec)) {
        fs::create_directories(app->sites_folder, ec);
    }

    std::vector<std::string> files;
    for (auto &entry : fs::directory_iterator(app->sites_folder, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;

        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                        [](unsigned char c) { return std::tolower(c); });
        if (ext == ".html") {
            files.push_back(entry.path().string());
        }
    }

    std::sort(files.begin(), files.end(), [](const std::string &a, const std::string &b) {
        return filename_without_ext(a) < filename_without_ext(b);
    });

    if (files.empty()) {
        std::string msg = "No .html files found in: " + app->sites_folder;
        GtkWidget *label = gtk_label_new(msg.c_str());
        gtk_widget_set_margin_top(label, 10);
        gtk_widget_set_margin_start(label, 10);
        gtk_box_append(GTK_BOX(app->results_box), label);
        return;
    }

    for (auto &file : files) {
        gtk_box_append(GTK_BOX(app->results_box), build_result_row(app, file));
    }
}

// ===================== Navigation =====================

static void navigate_browser(AppState *app, const std::string &file_path) {
    std::string uri = to_file_uri(file_path);
    if (uri.empty()) uri = file_path; // last-resort fallback
    webkit_web_view_load_uri(WEBKIT_WEB_VIEW(app->webview), uri.c_str());
}

static void show_results_panel(AppState *app, bool show) {
    // GtkPaned doesn't have a direct "collapse" concept like
    // SplitContainer.Panel1Collapsed, so we just hide/show the start
    // child widget directly.
    gtk_widget_set_visible(app->results_scroller, show);
}

static void update_nav_buttons(AppState *app) {
    gtk_widget_set_sensitive(app->back_btn, !app->back_history.empty());
    gtk_widget_set_sensitive(app->forward_btn, !app->forward_history.empty());
}

static void open_page(AppState *app, const std::string &file) {
    if (app->has_current_page) {
        app->back_history.push_back(app->current_page);
    }

    app->current_page = file;
    app->has_current_page = true;
    app->forward_history.clear();

    navigate_browser(app, file);

    show_results_panel(app, false);
    update_nav_buttons(app);
}

static void go_to_start_page(AppState *app) {
    std::error_code ec;
    if (!fs::exists(app->launch_page, ec)) {
        // Don't crash if it hasn't been created yet — just leave the
        // results list visible.
        show_results_panel(app, true);
        return;
    }

    if (app->has_current_page && app->current_page == app->launch_page) {
        return;
    }

    open_page(app, app->launch_page);
}

// ===================== Button handlers =====================

static void on_start_clicked(GtkButton * /*btn*/, gpointer user_data) {
    go_to_start_page(static_cast<AppState *>(user_data));
}

static void on_back_clicked(GtkButton * /*btn*/, gpointer user_data) {
    auto *app = static_cast<AppState *>(user_data);
    if (app->back_history.empty() || !app->has_current_page) return;

    app->forward_history.push_back(app->current_page);
    app->current_page = app->back_history.back();
    app->back_history.pop_back();

    navigate_browser(app, app->current_page);
    show_results_panel(app, false);
    update_nav_buttons(app);
}

static void on_forward_clicked(GtkButton * /*btn*/, gpointer user_data) {
    auto *app = static_cast<AppState *>(user_data);
    if (app->forward_history.empty() || !app->has_current_page) return;

    app->back_history.push_back(app->current_page);
    app->current_page = app->forward_history.back();
    app->forward_history.pop_back();

    navigate_browser(app, app->current_page);
    show_results_panel(app, false);
    update_nav_buttons(app);
}

static void on_reload_clicked(GtkButton * /*btn*/, gpointer user_data) {
    auto *app = static_cast<AppState *>(user_data);
    if (app->has_current_page) {
        navigate_browser(app, app->current_page);
    }
}

// Search always re-lists every .html file in sites/, regardless of the
// search box contents — matches the explicit earlier request that
// search should not filter anything.
static void on_search_clicked(GtkButton * /*btn*/, gpointer user_data) {
    auto *app = static_cast<AppState *>(user_data);
    show_results_panel(app, true);
    load_all_files(app);
}

static void on_search_entry_activate(GtkEntry * /*entry*/, gpointer user_data) {
    on_search_clicked(nullptr, user_data);
}

// ===================== UI construction =====================

static GtkWidget *make_toolbar(AppState *app) {
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(bar, 8);
    gtk_widget_set_margin_bottom(bar, 8);
    gtk_widget_set_margin_start(bar, 8);
    gtk_widget_set_margin_end(bar, 8);

    // START (leftmost — returns to the launch page)
    app->start_btn = gtk_button_new_with_label("Start");
    g_signal_connect(app->start_btn, "clicked", G_CALLBACK(on_start_clicked), app);

    // BACK
    app->back_btn = gtk_button_new_with_label("<");
    gtk_widget_set_sensitive(app->back_btn, FALSE);
    g_signal_connect(app->back_btn, "clicked", G_CALLBACK(on_back_clicked), app);

    // FORWARD
    app->forward_btn = gtk_button_new_with_label(">");
    gtk_widget_set_sensitive(app->forward_btn, FALSE);
    g_signal_connect(app->forward_btn, "clicked", G_CALLBACK(on_forward_clicked), app);

    // RELOAD
    app->reload_btn = gtk_button_new_with_label("R");
    g_signal_connect(app->reload_btn, "clicked", G_CALLBACK(on_reload_clicked), app);

    // SEARCH BOX
    app->search_entry = gtk_entry_new();
    gtk_widget_set_hexpand(app->search_entry, TRUE);
    g_signal_connect(app->search_entry, "activate", G_CALLBACK(on_search_entry_activate), app);

    // SEARCH BUTTON
    app->search_btn = gtk_button_new_with_label("Search");
    g_signal_connect(app->search_btn, "clicked", G_CALLBACK(on_search_clicked), app);

    gtk_box_append(GTK_BOX(bar), app->start_btn);
    gtk_box_append(GTK_BOX(bar), app->back_btn);
    gtk_box_append(GTK_BOX(bar), app->forward_btn);
    gtk_box_append(GTK_BOX(bar), app->reload_btn);
    gtk_box_append(GTK_BOX(bar), app->search_entry);
    gtk_box_append(GTK_BOX(bar), app->search_btn);

    return bar;
}

static void activate(GtkApplication *gtk_app, gpointer user_data) {
    auto *app = static_cast<AppState *>(user_data);

    app->window = gtk_application_window_new(gtk_app);
    gtk_window_set_title(GTK_WINDOW(app->window), "Internet Exploader");
    gtk_window_maximize(GTK_WINDOW(app->window));
    gtk_window_set_default_size(GTK_WINDOW(app->window), 800, 500);

    // Top-level vertical layout: toolbar on top, paned split filling
    // the rest. Unlike WinForms, GTK4 box packing order matches visual
    // order directly — no add-order surprises here.
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(app->window), root);

    GtkWidget *toolbar = make_toolbar(app);
    gtk_box_append(GTK_BOX(root), toolbar);

    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(root), separator);

    app->paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand(app->paned, TRUE);
    gtk_paned_set_position(GTK_PANED(app->paned), 350);
    gtk_box_append(GTK_BOX(root), app->paned);

    // ===== Results panel (left) =====
    app->results_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    app->results_scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(app->results_scroller),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(app->results_scroller), app->results_box);
    gtk_widget_set_size_request(app->results_scroller, 250, -1);

    gtk_paned_set_start_child(GTK_PANED(app->paned), app->results_scroller);
    gtk_paned_set_resize_start_child(GTK_PANED(app->paned), TRUE);
    gtk_paned_set_shrink_start_child(GTK_PANED(app->paned), FALSE);

    // ===== Browser (right) =====
    app->webview = GTK_WIDGET(webkit_web_view_new());
    gtk_widget_set_hexpand(app->webview, TRUE);
    gtk_widget_set_vexpand(app->webview, TRUE);

    gtk_paned_set_end_child(GTK_PANED(app->paned), app->webview);
    gtk_paned_set_resize_end_child(GTK_PANED(app->paned), TRUE);
    gtk_paned_set_shrink_end_child(GTK_PANED(app->paned), FALSE);

    // Results list visible on startup, same as the WinForms version.
    show_results_panel(app, true);
    load_all_files(app);

    // Land on IELAUNCHPAGE.html at startup instead of an empty browser pane.
    go_to_start_page(app);

    gtk_window_present(GTK_WINDOW(app->window));
}

// ===================== main =====================

int main(int argc, char **argv) {
    AppState app;

    // Folders sit next to the executable, same convention as the
    // WinForms version sitting next to the .exe.
    fs::path exe_dir = fs::canonical("/proc/self/exe").parent_path();
    app.sites_folder = (exe_dir / "sites").string();
    app.launch_page = (exe_dir / "LaunchAssets" / "IELAUNCHPAGE.html").string();

    GtkApplication *gtk_app = gtk_application_new(
        "org.internetexploader.browser", G_APPLICATION_DEFAULT_FLAGS);

    g_signal_connect(gtk_app, "activate", G_CALLBACK(activate), &app);

    int status = g_application_run(G_APPLICATION(gtk_app), argc, argv);
    g_object_unref(gtk_app);

    return status;
}
