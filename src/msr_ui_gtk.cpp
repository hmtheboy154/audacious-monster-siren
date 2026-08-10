/**
 * msr_ui_gtk.cpp - Audacious GTK UI/Catalog Plugin for Monster Siren (Arknights Soundtrack)
 *
 * Provides a dedicated GTK dockable window ("Monster Siren Catalog (GTK)") allowing users
 * running `audacious -i gtk` to browse, search, and play albums from the Monster Siren catalog.
 * Features:
 * 1. Asynchronous catalog loading and image decoding on background threads via `std::thread` + `g_idle_add()`.
 * 2. Instant search filtering via `GtkTreeModelFilter` across Album Title and Artistes.
 * 3. Multi-view support (Simple List, Album Cards, and Grid / Icon View) with seamless layout switching.
 * 4. Double-click activation that clears the active queue and starts playing (`msr://album/<cid>`).
 * 5. Full Audacious preferences integration sharing configuration with the Qt UI module under `[monster-siren]`.
 */

#include <string.h>
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <thread>
#include <vector>
#include <string>
#include <atomic>
#include <memory>

#define AUD_GLIB_INTEGRATION
#include <libaudcore/i18n.h>
#include <libaudcore/plugin.h>
#include <libaudcore/preferences.h>
#include <libaudcore/runtime.h>
#include <libaudcore/playlist.h>
#include <libaudcore/drct.h>

#include "monster_siren.h"

enum {
    COL_ICON = 0,
    COL_TITLE,
    COL_ARTIST,
    COL_CID,
    COL_COVER_URL,
    NUM_COLS
};

struct GtkCatalogContext {
    GtkWidget * main_box = nullptr;
    GtkWidget * search_entry = nullptr;
    GtkWidget * view_combo = nullptr;
    GtkWidget * stack = nullptr;
    GtkWidget * tree_view = nullptr;
    GtkWidget * icon_view = nullptr;
    GtkWidget * status_label = nullptr;
    
    GtkListStore * store = nullptr;
    GtkTreeModelFilter * filter = nullptr;
    
    std::string search_query;
    std::atomic<bool> destroyed{false};
    std::atomic<bool> loading_images{false};
};

static void on_window_enable_changed();

static const ComboItem view_mode_elems[] = {
    ComboItem(N_("Simple List"), 0),
    ComboItem(N_("Album List (with cover art)"), 1),
    ComboItem(N_("Grid / Icon View"), 2)
};

static const PreferencesWidget msr_gtk_widgets[] = {
    WidgetCheck(N_("Enable dockable catalog window"),
                WidgetBool("monster-siren", "enable_dedicated_window", on_window_enable_changed)),
    WidgetLabel(N_("<b>Catalog View & Layout</b>"), WIDGET_CHILD),
    WidgetCombo(N_("Default Catalog View Mode"),
                WidgetInt("monster-siren", "default_view_mode"),
                WidgetVCombo{{view_mode_elems}, nullptr}, WIDGET_CHILD),
    WidgetCheck(N_("Show album art icons in Simple List mode (uses more RAM)"),
                WidgetBool("monster-siren", "simple_mode_art"), WIDGET_CHILD),
    WidgetSeparator(),
    WidgetLabel(N_("<b>Disk Caching & Offline Indexing</b>")),
    WidgetCheck(N_("Enable Offline Disk Cache & Indexing"),
                WidgetBool("monster-siren", "enable_cache")),
    WidgetFileEntry(N_("Cache Directory"),
                    WidgetString("monster-siren", "cache_dir"),
                    {FileSelectMode::Folder}, WIDGET_CHILD),
    WidgetCheck(N_("Cache Catalog Metadata Index"),
                WidgetBool("monster-siren", "cache_catalog"), WIDGET_CHILD),
    WidgetCheck(N_("Cache Album Cover Art Images"),
                WidgetBool("monster-siren", "cache_images"), WIDGET_CHILD),
    WidgetCheck(N_("Cache Audio Tracks & Lyrics during playback"),
                WidgetBool("monster-siren", "cache_audio"), WIDGET_CHILD),
    WidgetCheck(N_("Manual Sync Mode (Load only from disk until forced refresh)"),
                WidgetBool("monster-siren", "manual_sync"), WIDGET_CHILD)
};

static const PluginPreferences msr_gtk_prefs = {.widgets = msr_gtk_widgets, .init = nullptr, .apply = nullptr, .cleanup = nullptr};

class MsrGtkPlugin : public GeneralPlugin
{
public:
    static constexpr PluginInfo info = {
        N_("Monster Siren Catalog (GTK)"),
        PACKAGE,
        nullptr, // about
        &msr_gtk_prefs,
        PluginGLibOnly
    };

    constexpr MsrGtkPlugin() : GeneralPlugin(info, true) {}

    bool init() override { return true; }
    void cleanup() override {}
    void * get_gtk_widget() override;
};

EXPORT MsrGtkPlugin aud_plugin_instance;

static void on_window_enable_changed()
{
}

static gboolean search_filter_func(GtkTreeModel * model, GtkTreeIter * iter, gpointer data)
{
    GtkCatalogContext * ctx = (GtkCatalogContext *)data;
    if (!ctx || ctx->search_query.empty())
        return TRUE;

    gchar * title = nullptr;
    gchar * artist = nullptr;
    gtk_tree_model_get(model, iter, COL_TITLE, &title, COL_ARTIST, &artist, -1);

    gboolean matches = FALSE;
    gchar * query_lower = g_utf8_strdown(ctx->search_query.c_str(), -1);

    if (title) {
        gchar * title_lower = g_utf8_strdown(title, -1);
        if (strstr(title_lower, query_lower))
            matches = TRUE;
        g_free(title_lower);
    }
    if (!matches && artist) {
        gchar * artist_lower = g_utf8_strdown(artist, -1);
        if (strstr(artist_lower, query_lower))
            matches = TRUE;
        g_free(artist_lower);
    }

    g_free(query_lower);
    if (title) g_free(title);
    if (artist) g_free(artist);
    return matches;
}

static void on_search_changed(GtkSearchEntry * entry, gpointer data)
{
    GtkCatalogContext * ctx = (GtkCatalogContext *)data;
    if (!ctx || !ctx->filter)
        return;

    const gchar * text = gtk_entry_get_text(GTK_ENTRY(entry));
    ctx->search_query = text ? text : "";
    gtk_tree_model_filter_refilter(ctx->filter);
}

static void update_view_mode(GtkCatalogContext * ctx, int mode)
{
    if (!ctx || !ctx->stack)
        return;

    if (mode == 2) {
        gtk_stack_set_visible_child_name(GTK_STACK(ctx->stack), "grid");
    } else {
        gtk_stack_set_visible_child_name(GTK_STACK(ctx->stack), "list");
    }
}

static void on_view_combo_changed(GtkComboBox * combo, gpointer data)
{
    GtkCatalogContext * ctx = (GtkCatalogContext *)data;
    if (!ctx)
        return;

    int mode = gtk_combo_box_get_active(combo);
    aud_set_int("monster-siren", "default_view_mode", mode);
    update_view_mode(ctx, mode);
}

static void play_album_by_cid(const gchar * cid)
{
    if (!cid || !*cid)
        return;

    AUDINFO("MSR-GTK: Selected album CID: %s -> clearing queue & starting playback\n", cid);
    Playlist active = Playlist::active_playlist();
    active.remove_all_entries();

    Index<PlaylistAddItem> items;
    std::string url = std::string("msr://album/") + cid;
    items.append(String(url.c_str()));

    active.insert_items(0, std::move(items), true);
}

static void on_tree_row_activated(GtkTreeView * tree_view, GtkTreePath * path, GtkTreeViewColumn * column, gpointer data)
{
    (void)tree_view;
    (void)column;
    GtkCatalogContext * ctx = (GtkCatalogContext *)data;
    if (!ctx || !ctx->filter)
        return;

    GtkTreeIter filter_iter, store_iter;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(ctx->filter), &filter_iter, path))
        return;

    gtk_tree_model_filter_convert_iter_to_child_iter(ctx->filter, &store_iter, &filter_iter);
    gchar * cid = nullptr;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->store), &store_iter, COL_CID, &cid, -1);

    if (cid) {
        play_album_by_cid(cid);
        g_free(cid);
    }
}

static void on_icon_item_activated(GtkIconView * icon_view, GtkTreePath * path, gpointer data)
{
    (void)icon_view;
    GtkCatalogContext * ctx = (GtkCatalogContext *)data;
    if (!ctx || !ctx->filter)
        return;

    GtkTreeIter filter_iter, store_iter;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(ctx->filter), &filter_iter, path))
        return;

    gtk_tree_model_filter_convert_iter_to_child_iter(ctx->filter, &store_iter, &filter_iter);
    gchar * cid = nullptr;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->store), &store_iter, COL_CID, &cid, -1);

    if (cid) {
        play_album_by_cid(cid);
        g_free(cid);
    }
}

struct ImageUpdateData {
    GtkCatalogContext * ctx;
    std::string cid;
    GdkPixbuf * pixbuf;
};

static gboolean on_image_loaded_idle(gpointer user_data)
{
    ImageUpdateData * update = (ImageUpdateData *)user_data;
    if (!update)
        return FALSE;

    if (!update->ctx->destroyed && update->ctx->store && update->pixbuf) {
        GtkTreeIter iter;
        gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(update->ctx->store), &iter);
        while (valid) {
            gchar * row_cid = nullptr;
            gtk_tree_model_get(GTK_TREE_MODEL(update->ctx->store), &iter, COL_CID, &row_cid, -1);
            if (row_cid && update->cid == row_cid) {
                gtk_list_store_set(update->ctx->store, &iter, COL_ICON, update->pixbuf, -1);
                g_free(row_cid);
                break;
            }
            if (row_cid) g_free(row_cid);
            valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(update->ctx->store), &iter);
        }
    }

    if (update->pixbuf)
        g_object_unref(update->pixbuf);
    delete update;
    return FALSE;
}

static void start_async_image_loader(GtkCatalogContext * ctx)
{
    if (!ctx || ctx->loading_images.exchange(true))
        return;

    struct CoverInfo { std::string cid; std::string url; };
    std::vector<CoverInfo> covers;

    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(ctx->store), &iter);
    while (valid) {
        gchar * cid = nullptr;
        gchar * url = nullptr;
        gtk_tree_model_get(GTK_TREE_MODEL(ctx->store), &iter, COL_CID, &cid, COL_COVER_URL, &url, -1);
        if (cid && url && *url)
            covers.push_back({cid, url});
        if (cid) g_free(cid);
        if (url) g_free(url);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(ctx->store), &iter);
    }

    std::thread([ctx, covers = std::move(covers)]() {
        for (const auto & info : covers) {
            if (ctx->destroyed) break;

            Index<char> img_bytes = msr_fetch_image(String(info.url.c_str()));
            if (ctx->destroyed || !img_bytes.len()) continue;

            GdkPixbufLoader * loader = gdk_pixbuf_loader_new();
            gdk_pixbuf_loader_write(loader, (const guchar *)img_bytes.begin(), img_bytes.len(), nullptr);
            gdk_pixbuf_loader_close(loader, nullptr);
            GdkPixbuf * raw_pix = gdk_pixbuf_loader_get_pixbuf(loader);

            if (raw_pix) {
                GdkPixbuf * scaled = gdk_pixbuf_scale_simple(raw_pix, 150, 150, GDK_INTERP_BILINEAR);
                g_object_unref(loader);

                if (scaled) {
                    ImageUpdateData * update = new ImageUpdateData{ctx, info.cid, scaled};
                    g_idle_add(on_image_loaded_idle, update);
                }
            } else {
                g_object_unref(loader);
            }
        }
    }).detach();
}

struct CatalogPopulateData {
    GtkCatalogContext * ctx;
    std::vector<MsrAlbum> albums;
};

static gboolean on_catalog_loaded_idle(gpointer user_data)
{
    CatalogPopulateData * data = (CatalogPopulateData *)user_data;
    if (!data)
        return FALSE;

    GtkCatalogContext * ctx = data->ctx;
    if (!ctx->destroyed && ctx->store) {
        gtk_list_store_clear(ctx->store);

        for (const auto & alb : data->albums) {
            GtkTreeIter iter;
            gtk_list_store_append(ctx->store, &iter);
            gtk_list_store_set(ctx->store, &iter,
                               COL_ICON, nullptr,
                               COL_TITLE, (const char *)alb.name,
                               COL_ARTIST, (const char *)alb.artistes,
                               COL_CID, (const char *)alb.cid,
                               COL_COVER_URL, (const char *)alb.coverUrl,
                               -1);
        }

        if (ctx->status_label) {
            char status_buf[256];
            snprintf(status_buf, sizeof(status_buf),
                     _("Catalog loaded: %zu albums.\nDouble-click to clear queue & play."),
                     data->albums.size());
            gtk_label_set_text(GTK_LABEL(ctx->status_label), status_buf);
        }

        int mode = aud_get_int("monster-siren", "default_view_mode");
        bool load_images = (mode > 0) || aud_get_bool("monster-siren", "simple_mode_art");
        if (load_images)
            start_async_image_loader(ctx);
    }

    delete data;
    return FALSE;
}

static void fetch_and_populate_catalog(GtkCatalogContext * ctx, bool force_network)
{
    if (!ctx)
        return;

    if (ctx->status_label)
        gtk_label_set_text(GTK_LABEL(ctx->status_label), _("Loading Monster Siren catalog..."));

    std::thread([ctx, force_network]() {
        std::vector<MsrAlbum> albums = msr_fetch_albums(force_network);
        if (ctx->destroyed)
            return;

        CatalogPopulateData * data = new CatalogPopulateData{ctx, std::move(albums)};
        g_idle_add(on_catalog_loaded_idle, data);
    }).detach();
}

static void on_refresh_clicked(GtkButton * btn, gpointer data)
{
    (void)btn;
    GtkCatalogContext * ctx = (GtkCatalogContext *)data;
    fetch_and_populate_catalog(ctx, true);
}

static void on_main_box_destroy(GtkWidget * widget, gpointer data)
{
    (void)widget;
    GtkCatalogContext * ctx = (GtkCatalogContext *)data;
    if (ctx) {
        ctx->destroyed = true;
        delete ctx;
    }
}

void * MsrGtkPlugin::get_gtk_widget()
{
    if (!aud_get_bool("monster-siren", "enable_dedicated_window"))
        return nullptr;

    GtkCatalogContext * ctx = new GtkCatalogContext();
    ctx->main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    g_signal_connect(ctx->main_box, "destroy", G_CALLBACK(on_main_box_destroy), ctx);

    // Top control bar
    GtkWidget * top_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(ctx->main_box), top_box, FALSE, FALSE, 0);

    ctx->search_entry = gtk_search_entry_new();
    gtk_widget_set_tooltip_text(ctx->search_entry, _("Search albums by title or artist..."));
    g_signal_connect(ctx->search_entry, "search-changed", G_CALLBACK(on_search_changed), ctx);
    gtk_box_pack_start(GTK_BOX(top_box), ctx->search_entry, TRUE, TRUE, 0);

    ctx->view_combo = gtk_combo_box_text_new();
    for (const auto & elem : view_mode_elems)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ctx->view_combo), _(elem.label));
    
    int current_mode = aud_get_int("monster-siren", "default_view_mode");
    if (current_mode < 0 || current_mode > 2) current_mode = 0;
    gtk_combo_box_set_active(GTK_COMBO_BOX(ctx->view_combo), current_mode);
    g_signal_connect(ctx->view_combo, "changed", G_CALLBACK(on_view_combo_changed), ctx);
    gtk_box_pack_start(GTK_BOX(top_box), ctx->view_combo, FALSE, FALSE, 0);

    GtkWidget * refresh_btn = gtk_button_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(refresh_btn, _("Force refresh catalog from network"));
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK(on_refresh_clicked), ctx);
    gtk_box_pack_start(GTK_BOX(top_box), refresh_btn, FALSE, FALSE, 0);

    // Scrolled window holding view stack
    GtkWidget * scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(ctx->main_box), scroll, TRUE, TRUE, 0);

    ctx->store = gtk_list_store_new(NUM_COLS, GDK_TYPE_PIXBUF, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    ctx->filter = GTK_TREE_MODEL_FILTER(gtk_tree_model_filter_new(GTK_TREE_MODEL(ctx->store), nullptr));
    gtk_tree_model_filter_set_visible_func(ctx->filter, search_filter_func, ctx, nullptr);

    ctx->stack = gtk_stack_new();
    gtk_container_add(GTK_CONTAINER(scroll), ctx->stack);

    // 1. TreeView (List / Album Cards)
    ctx->tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ctx->filter));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(ctx->tree_view), FALSE);
    g_signal_connect(ctx->tree_view, "row-activated", G_CALLBACK(on_tree_row_activated), ctx);

    GtkCellRenderer * pix_rend = gtk_cell_renderer_pixbuf_new();
    GtkTreeViewColumn * col_icon = gtk_tree_view_column_new_with_attributes("", pix_rend, "pixbuf", COL_ICON, nullptr);
    gtk_tree_view_append_column(GTK_TREE_VIEW(ctx->tree_view), col_icon);

    GtkCellRenderer * txt_rend = gtk_cell_renderer_text_new();
    GtkTreeViewColumn * col_txt = gtk_tree_view_column_new();
    gtk_tree_view_column_pack_start(col_txt, txt_rend, TRUE);
    gtk_tree_view_column_add_attribute(col_txt, txt_rend, "text", COL_TITLE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(ctx->tree_view), col_txt);

    GtkCellRenderer * artist_rend = gtk_cell_renderer_text_new();
    g_object_set(artist_rend, "foreground", "gray", nullptr);
    GtkTreeViewColumn * col_artist = gtk_tree_view_column_new();
    gtk_tree_view_column_pack_start(col_artist, artist_rend, TRUE);
    gtk_tree_view_column_add_attribute(col_artist, artist_rend, "text", COL_ARTIST);
    gtk_tree_view_append_column(GTK_TREE_VIEW(ctx->tree_view), col_artist);

    gtk_stack_add_named(GTK_STACK(ctx->stack), ctx->tree_view, "list");

    // 2. IconView (Grid View)
    ctx->icon_view = gtk_icon_view_new_with_model(GTK_TREE_MODEL(ctx->filter));
    gtk_icon_view_set_pixbuf_column(GTK_ICON_VIEW(ctx->icon_view), COL_ICON);
    gtk_icon_view_set_text_column(GTK_ICON_VIEW(ctx->icon_view), COL_TITLE);
    gtk_icon_view_set_item_width(GTK_ICON_VIEW(ctx->icon_view), 160);
    g_signal_connect(ctx->icon_view, "item-activated", G_CALLBACK(on_icon_item_activated), ctx);

    gtk_stack_add_named(GTK_STACK(ctx->stack), ctx->icon_view, "grid");

    update_view_mode(ctx, current_mode);

    // Status label
    ctx->status_label = gtk_label_new(_("Initializing Monster Siren catalog..."));
    gtk_label_set_xalign(GTK_LABEL(ctx->status_label), 0.0f);
    gtk_box_pack_start(GTK_BOX(ctx->main_box), ctx->status_label, FALSE, FALSE, 2);

    gtk_widget_show_all(ctx->main_box);

    fetch_and_populate_catalog(ctx, false);

    return ctx->main_box;
}
