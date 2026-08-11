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
#include <libaudcore/plugins.h>
#include <libaudcore/hook.h>
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
    COL_ICON_SMALL,
    NUM_COLS
};

struct GtkCatalogContext {
    GtkWidget * main_box = nullptr;
    GtkWidget * search_entry = nullptr;
    GtkWidget * view_combo = nullptr;
    gulong view_combo_handler = 0;
    GtkWidget * scroll = nullptr;
    GtkWidget * tree_view = nullptr;
    GtkWidget * status_label = nullptr;

    GtkListStore * store = nullptr;
    GtkTreeModelFilter * filter = nullptr;

    std::string search_query;
    std::atomic<bool> destroyed{false};
    std::atomic<bool> loading_images{false};
    int current_mode = -1;
    bool current_simple_mode_art = false;
};

/* Live plugin window contexts; used to apply preference changes immediately. */
static std::vector<std::shared_ptr<GtkCatalogContext>> g_gtk_ctxs;

static void on_tree_row_activated(GtkTreeView * tree_view, GtkTreePath * path,
                                  GtkTreeViewColumn * column, gpointer data);
static void on_icon_item_activated(GtkIconView * icon_view, GtkTreePath * path, gpointer data);
static void start_async_image_loader(std::shared_ptr<GtkCatalogContext> ctx);
static void msr_gtk_prefs_apply();

static void on_window_enable_changed();

static const ComboItem view_mode_elems[] = {
    ComboItem(N_("Simple List"), 0),
    ComboItem(N_("Album List (with cover art)"), 1),
    ComboItem(N_("Grid / Icon View"), 2)
};

static const PreferencesWidget msr_gtk_widgets[] = {
    WidgetCheck(N_("Enable dockable catalog window"),
                WidgetBool("monster-siren", "enable_dedicated_window", on_window_enable_changed)),
    WidgetLabel(N_("<b>Catalog View &amp; Layout</b>"), WIDGET_CHILD),
    WidgetCombo(N_("Default Catalog View Mode"),
                WidgetInt("monster-siren", "default_view_mode"),
                WidgetVCombo{{view_mode_elems}, nullptr}, WIDGET_CHILD),
    WidgetCheck(N_("Show album art icons in Simple List mode (uses more RAM)"),
                WidgetBool("monster-siren", "simple_mode_art"), WIDGET_CHILD)
};

static const PluginPreferences msr_gtk_prefs = {.widgets = msr_gtk_widgets, .init = nullptr, .apply = msr_gtk_prefs_apply, .cleanup = nullptr};

/* The Qt UI module registers these defaults, but it is PluginQtOnly and never
 * loads in GTK mode, leaving the shared [monster-siren] options unregistered
 * here. Register the view-mode keys so both prefs widgets read/write
 * consistently; the disk caching keys are owned by the input plugin. */
static const char * const msr_gtk_defaults[] = {
    "enable_dedicated_window", "TRUE",
    "simple_mode_art", "FALSE",
    "default_view_mode", "0",
    nullptr
};

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

    bool init() override
    {
        aud_config_set_defaults("monster-siren", msr_gtk_defaults);
        return true;
    }

    /* Called while the plugin module is still loaded, before General plugin
     * teardown dlcloses it. Waits for any catalog/image workers so they can
     * never execute into freed transport code after module unload. */
    void cleanup() override { msr_wait_for_background_threads(); }
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

/* The scrolled window is a GtkBin: it holds exactly one child at a time, so
 * a view switch deterministically swaps the visible page instead of relying
 * on GtkStack page bookkeeping that could go stale. */
static void set_scroll_child(GtkCatalogContext * ctx, GtkWidget * child)
{
    if (!ctx || !ctx->scroll || !child)
        return;

    GtkWidget * old = gtk_bin_get_child(GTK_BIN(ctx->scroll));
    if (old != child) {
        if (old)
            gtk_container_remove(GTK_CONTAINER(ctx->scroll), old);
        gtk_container_add(GTK_CONTAINER(ctx->scroll), child);
        gtk_widget_show(child);
    }
}

/* Builds the list page (Simple List / Album List) from scratch. A fresh
 * GtkTreeView is required on every mode switch: GtkTreeView caches row
 * heights from its initial layout and never re-layouts rows when a renderer
 * is rebound to a different (smaller/larger) pixbuf column at runtime, which
 * caused sticky column/row sizes and clipped art. */
static void build_list_view(GtkCatalogContext * ctx, int mode)
{
    if (!ctx || !ctx->scroll || !ctx->filter)
        return;

    bool simple_mode = (mode == 0);
    bool show_art = !simple_mode || aud_get_bool("monster-siren", "simple_mode_art");

    GtkWidget * tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ctx->filter));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree), FALSE);
    g_signal_connect(tree, "row-activated", G_CALLBACK(on_tree_row_activated), ctx);

    if (show_art) {
        /* Simple List renders the small thumbnail column, Album List the full
         * size one; the column only exists when art is actually shown. */
        GtkCellRenderer * pix_rend = gtk_cell_renderer_pixbuf_new();
        GtkTreeViewColumn * col_icon = gtk_tree_view_column_new_with_attributes(
            "", pix_rend, "pixbuf", simple_mode ? COL_ICON_SMALL : COL_ICON, nullptr);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col_icon);
    }

    GtkCellRenderer * txt_rend = gtk_cell_renderer_text_new();
    GtkTreeViewColumn * col_txt = gtk_tree_view_column_new();
    gtk_tree_view_column_pack_start(col_txt, txt_rend, TRUE);
    gtk_tree_view_column_add_attribute(col_txt, txt_rend, "text", COL_TITLE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col_txt);

    GtkCellRenderer * artist_rend = gtk_cell_renderer_text_new();
    g_object_set(artist_rend, "foreground", "gray", nullptr);
    GtkTreeViewColumn * col_artist = gtk_tree_view_column_new();
    gtk_tree_view_column_pack_start(col_artist, artist_rend, TRUE);
    gtk_tree_view_column_add_attribute(col_artist, artist_rend, "text", COL_ARTIST);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col_artist);

    ctx->tree_view = tree;
    set_scroll_child(ctx, tree);
    gtk_tree_view_columns_autosize(GTK_TREE_VIEW(tree));
}

/* Builds the grid (Icon View) page from scratch on every switch, mirroring
 * build_list_view(): GtkIconView also caches layout metrics, so reusing the
 * widget can leave stale row/column sizing after a mode change. */
static void build_grid_page(GtkCatalogContext * ctx)
{
    if (!ctx || !ctx->scroll || !ctx->filter)
        return;

    GtkWidget * icon_view = gtk_icon_view_new_with_model(GTK_TREE_MODEL(ctx->filter));
    gtk_icon_view_set_pixbuf_column(GTK_ICON_VIEW(icon_view), COL_ICON);
    gtk_icon_view_set_text_column(GTK_ICON_VIEW(icon_view), COL_TITLE);
    gtk_icon_view_set_item_width(GTK_ICON_VIEW(icon_view), 160);

    /* GtkIconView sizes itself to its natural width; with everything in one
     * or two rows the scrolled window would scroll horizontally instead of
     * vertically. Constrain it to the allocated width so items flow into rows
     * that scroll up/down. */
    gtk_widget_set_size_request(icon_view, 1, -1);
    gtk_widget_set_hexpand(icon_view, TRUE);
    gtk_widget_set_halign(icon_view, GTK_ALIGN_FILL);

    g_signal_connect(icon_view, "item-activated", G_CALLBACK(on_icon_item_activated), ctx);

    set_scroll_child(ctx, icon_view);
}

static void maybe_start_image_loader(GtkCatalogContext * ctx, int mode)
{
    if (!ctx)
        return;

    bool needs_images = (mode > 0) || aud_get_bool("monster-siren", "simple_mode_art");
    if (!needs_images)
        return;

    /* start_async_image_loader needs the shared_ptr to keep the context alive
     * while the worker runs; every live context is tracked in g_gtk_ctxs. */
    for (auto & shared : g_gtk_ctxs) {
        if (shared.get() == ctx) {
            start_async_image_loader(shared);
            break;
        }
    }
}

static void update_gtk_view_mode(GtkCatalogContext * ctx, int mode)
{
    if (!ctx || !ctx->scroll)
        return;

    bool mode_changed = (ctx->current_mode != mode);
    bool art_changed = (ctx->current_simple_mode_art !=
                        aud_get_bool("monster-siren", "simple_mode_art"));
    ctx->current_mode = mode;
    ctx->current_simple_mode_art = aud_get_bool("monster-siren", "simple_mode_art");

    AUDWARN("MSR-GTK: update_gtk_view_mode mode=%d (mode_changed=%d art_changed=%d)\n",
            mode, mode_changed, art_changed);

    if (mode == 2) {
        if (mode_changed)
            build_grid_page(ctx);
        AUDWARN("MSR-GTK: page attached -> grid\n");
    } else if (mode_changed || art_changed || !ctx->tree_view) {
        build_list_view(ctx, mode);
        AUDWARN("MSR-GTK: page attached -> list\n");
    } else {
        AUDWARN("MSR-GTK: page unchanged, keeping current list page\n");
    }

    maybe_start_image_loader(ctx, mode);
}

static void on_view_combo_changed(GtkComboBox * combo, gpointer data)
{
    GtkCatalogContext * ctx = (GtkCatalogContext *)data;
    if (!ctx)
        return;

    int mode = gtk_combo_box_get_active(combo);
    AUDWARN("MSR-GTK: view combo changed -> mode=%d\n", mode);
    aud_set_int("monster-siren", "default_view_mode", mode);
    update_gtk_view_mode(ctx, mode);
}

/* Applies the current [monster-siren] settings to one live window, keeping the
 * in-window combo in sync with what the preferences dialog may have changed. */
static void apply_view_mode_to_window(GtkCatalogContext * ctx, int mode)
{
    if (!ctx || ctx->destroyed)
        return;

    if (ctx->view_combo && ctx->view_combo_handler) {
        g_signal_handler_block(ctx->view_combo, ctx->view_combo_handler);
        gtk_combo_box_set_active(GTK_COMBO_BOX(ctx->view_combo), mode);
        g_signal_handler_unblock(ctx->view_combo, ctx->view_combo_handler);
    }

    update_gtk_view_mode(ctx, mode);
}

static void msr_gtk_prefs_apply()
{
    bool want_window = aud_get_bool("monster-siren", "enable_dedicated_window");
    bool have_window = !g_gtk_ctxs.empty();
    AUDWARN("MSR-GTK: preferences applied (want_window=%d have_window=%d)\n",
            want_window, have_window);

    if (want_window && !have_window) {
        /* The interface (gtkui/qt) only creates dockable-plugin windows when it
         * receives the "dock plugin enabled" hook, so re-fire it to restore a
         * window that was lost (e.g. after toggling the plugin list entry). */
        PluginHandle * self = aud_plugin_by_header(&aud_plugin_instance);
        if (aud_plugin_get_enabled(self))
            hook_call("dock plugin enabled", self);
        return; /* get_gtk_widget() applies the current view mode itself */
    }

    if (!want_window && have_window) {
        PluginHandle * self = aud_plugin_by_header(&aud_plugin_instance);
        hook_call("dock plugin disabled", self);
        return; /* on_main_box_destroy() drops each context from the registry */
    }

    for (auto & ctx : g_gtk_ctxs)
        apply_view_mode_to_window(ctx.get(), aud_get_int("monster-siren", "default_view_mode"));
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
    std::shared_ptr<GtkCatalogContext> ctx;
    std::string cid;
    GdkPixbuf * pixbuf;
};

static gboolean on_image_loaded_idle(gpointer user_data)
{
    ImageUpdateData * update = (ImageUpdateData *)user_data;
    std::shared_ptr<GtkCatalogContext> ctx = update->ctx;
    if (!update->pixbuf)
    {
        delete update;
        return FALSE;
    }

    if (!ctx->destroyed && ctx->store) {
        GtkTreeIter iter;
        gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(update->ctx->store), &iter);
        while (valid) {
            gchar * row_cid = nullptr;
            gtk_tree_model_get(GTK_TREE_MODEL(update->ctx->store), &iter, COL_CID, &row_cid, -1);
            if (row_cid && update->cid == row_cid) {
                /* Big art for Album List/Grid, small thumbnail for Simple List */
                GdkPixbuf * small = gdk_pixbuf_scale_simple(update->pixbuf, 32, 32, GDK_INTERP_BILINEAR);
                gtk_list_store_set(update->ctx->store, &iter,
                                   COL_ICON, update->pixbuf,
                                   COL_ICON_SMALL, small,
                                   -1);
                if (small)
                    g_object_unref(small);
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

static void start_async_image_loader(std::shared_ptr<GtkCatalogContext> ctx)
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

    msr_run_background([ctx, covers = std::move(covers)]() {
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
    });
}

struct CatalogPopulateData {
    std::shared_ptr<GtkCatalogContext> ctx;
    std::vector<MsrAlbum> albums;
};

static gboolean on_catalog_loaded_idle(gpointer user_data)
{
    CatalogPopulateData * data = (CatalogPopulateData *)user_data;
    GtkCatalogContext * ctx = data->ctx.get();
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
                               COL_ICON_SMALL, nullptr,
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
        maybe_start_image_loader(data->ctx.get(), mode);
    }

    delete data;
    return FALSE;
}

static void fetch_and_populate_catalog(std::shared_ptr<GtkCatalogContext> ctx, bool force_network)
{
    if (!ctx)
        return;

    if (ctx->status_label)
        gtk_label_set_text(GTK_LABEL(ctx->status_label), _("Loading Monster Siren catalog..."));

    msr_run_background([ctx, force_network]() {
        std::vector<MsrAlbum> albums = msr_fetch_albums(force_network);
        if (ctx->destroyed)
            return;

        CatalogPopulateData * data = new CatalogPopulateData{ctx, std::move(albums)};
        g_idle_add(on_catalog_loaded_idle, data);
    });
}

static void on_refresh_clicked(GtkButton * btn, gpointer data)
{
    (void)data;
    auto * ps = (std::shared_ptr<GtkCatalogContext> *)g_object_get_data(G_OBJECT(btn), "msr-ctx-shared");
    if (!ps)
        return;

    std::shared_ptr<GtkCatalogContext> ctx = *ps;
    msr_invalidate_catalog_cache();
    fetch_and_populate_catalog(ctx, true);
}

static void on_main_box_destroy(GtkWidget * widget, gpointer data)
{
    (void)data;
    /* The GObject data (and with it the main shared_ptr reference) is
     * released right after this signal is emitted; any background workers or
     * queued idle callbacks keep the context alive through their own
     * references, so we only need to flag the context as destroyed here. */
    auto * ps = (std::shared_ptr<GtkCatalogContext> *)g_object_get_data(G_OBJECT(widget), "msr-ctx");
    if (ps) {
        AUDWARN("MSR-GTK: dock window destroyed\n");
        (*ps)->destroyed = true;
        for (auto it = g_gtk_ctxs.begin(); it != g_gtk_ctxs.end(); ++it) {
            if (it->get() == ps->get()) {
                g_gtk_ctxs.erase(it);
                break;
            }
        }
    }
}

void * MsrGtkPlugin::get_gtk_widget()
{
    if (!aud_get_bool("monster-siren", "enable_dedicated_window"))
        return nullptr;

    auto ctx = std::make_shared<GtkCatalogContext>();
    GtkCatalogContext * c = ctx.get();
    c->main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    g_signal_connect(c->main_box, "destroy", G_CALLBACK(on_main_box_destroy), nullptr);

    /* Keep one shared_ptr reference tied to the widget lifetime; workers and
     * idle callbacks hold their own copies, so the context is never deleted
     * while they are still using it. */
    g_object_set_data_full(G_OBJECT(c->main_box), "msr-ctx",
                           new std::shared_ptr<GtkCatalogContext>(ctx),
                           [](gpointer p) { delete (std::shared_ptr<GtkCatalogContext> *)p; });

    // Top control bar
    GtkWidget * top_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(c->main_box), top_box, FALSE, FALSE, 0);

    c->search_entry = gtk_search_entry_new();
    gtk_widget_set_tooltip_text(c->search_entry, _("Search albums by title or artist..."));
    g_signal_connect(c->search_entry, "search-changed", G_CALLBACK(on_search_changed), c);
    gtk_box_pack_start(GTK_BOX(top_box), c->search_entry, TRUE, TRUE, 0);

    c->view_combo = gtk_combo_box_text_new();
    for (const auto & elem : view_mode_elems)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c->view_combo), _(elem.label));
    
    int current_mode = aud_get_int("monster-siren", "default_view_mode");
    if (current_mode < 0 || current_mode > 2) current_mode = 0;
    AUDWARN("MSR-GTK: startup default_view_mode=%d\n", current_mode);

    /* Set the initial combo item without firing the "changed" handler so the
     * disk value is applied exactly once, deterministically, below. */
    c->view_combo_handler = g_signal_connect(c->view_combo, "changed", G_CALLBACK(on_view_combo_changed), c);
    g_signal_handler_block(c->view_combo, c->view_combo_handler);
    gtk_combo_box_set_active(GTK_COMBO_BOX(c->view_combo), current_mode);
    g_signal_handler_unblock(c->view_combo, c->view_combo_handler);
    gtk_box_pack_start(GTK_BOX(top_box), c->view_combo, FALSE, FALSE, 0);

    GtkWidget * refresh_btn = gtk_button_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(refresh_btn, _("Force refresh catalog from network"));
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK(on_refresh_clicked), nullptr);
    g_object_set_data_full(G_OBJECT(refresh_btn), "msr-ctx-shared",
                           new std::shared_ptr<GtkCatalogContext>(ctx),
                           [](gpointer p) { delete (std::shared_ptr<GtkCatalogContext> *)p; });
    gtk_box_pack_start(GTK_BOX(top_box), refresh_btn, FALSE, FALSE, 0);

    // Scrolled window holding the current view page (one child at a time)
    GtkWidget * scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(c->main_box), scroll, TRUE, TRUE, 0);
    c->scroll = scroll;

    c->store = gtk_list_store_new(NUM_COLS, GDK_TYPE_PIXBUF, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, GDK_TYPE_PIXBUF);
    c->filter = GTK_TREE_MODEL_FILTER(gtk_tree_model_filter_new(GTK_TREE_MODEL(c->store), nullptr));
    gtk_tree_model_filter_set_visible_func(c->filter, search_filter_func, c, nullptr);

    /* The current page (Simple/Album List or Grid) is attached to the scrolled
     * window by update_gtk_view_mode() below. */

    update_gtk_view_mode(c, current_mode);

    // Status label
    c->status_label = gtk_label_new(_("Initializing Monster Siren catalog..."));
    gtk_label_set_xalign(GTK_LABEL(c->status_label), 0.0f);
    gtk_box_pack_start(GTK_BOX(c->main_box), c->status_label, FALSE, FALSE, 2);

    gtk_widget_show_all(c->main_box);

    g_gtk_ctxs.push_back(ctx);

    fetch_and_populate_catalog(ctx, false);

    return c->main_box;
}
