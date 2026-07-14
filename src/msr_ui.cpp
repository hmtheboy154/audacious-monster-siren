/**
 * msr_ui.cpp - Audacious UI/Catalog Plugin for Monster Siren (Arknights Soundtrack)
 *
 * Provides a dedicated Qt dockable window ("Monster Siren Catalog") allowing users
 * to browse, search, and play albums from the Monster Siren catalog. Features:
 * 1. Asynchronous catalog loading on a background thread to prevent GUI/startup freeze.
 * 2. QSortFilterProxyModel for instant real-time searching across Album Title and Artistes.
 * 3. Double-click activation that clears the current queue and plays the chosen album (`msr://album/<cid>`).
 * 4. Integration with Audacious preferences (`enable_dedicated_window`) to show/hide the dock panel.
 */

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeView>
#include <QListView>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QIcon>
#include <QImage>
#include <QStandardItemModel>
#include <QSortFilterProxyModel>
#include <QHeaderView>
#include <QLabel>
#include <QApplication>
#include <QPointer>
#include <thread>

#include "monster_siren.h"
#include <libaudcore/plugin.h>
#include <libaudcore/i18n.h>
#include <libaudcore/preferences.h>
#include <libaudcore/runtime.h>
#include <libaudcore/hook.h>
#include <libaudcore/playlist.h>
#include <libaudcore/drct.h>

class MsrUiPlugin;
extern MsrUiPlugin aud_plugin_instance;

static const char * const msr_ui_defaults[] = {
    "enable_dedicated_window", "TRUE",
    "simple_mode_art", "FALSE",
    "enable_cache", "FALSE",
    "cache_dir", "",
    "cache_catalog", "TRUE",
    "cache_images", "TRUE",
    "cache_audio", "TRUE",
    "manual_sync", "FALSE",
    "default_view_mode", "0",
    nullptr
};

static void on_window_enable_changed();

static const ComboItem view_mode_elems[] = {
    ComboItem(N_("Simple List"), 0),
    ComboItem(N_("Album List (with cover art)"), 1),
    ComboItem(N_("Grid / Icon View"), 2)
};

static const PreferencesWidget msr_ui_widgets[] = {
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

static const PluginPreferences msr_ui_prefs = {.widgets = msr_ui_widgets, .init = nullptr, .apply = nullptr, .cleanup = nullptr};

class MsrUiPlugin : public GeneralPlugin
{
public:
    static constexpr PluginInfo info = {
        N_("Monster Siren Catalog"),
        PACKAGE,
        nullptr, // about
        &msr_ui_prefs,
        PluginQtOnly
    };

    constexpr MsrUiPlugin()
        : GeneralPlugin(info, false)
    {
    }

    bool init() override
    {
        aud_config_set_defaults("monster-siren", msr_ui_defaults);
        msr_get_cache_dir(); // populate default cache dir right at startup for UI display
        return true;
    }

    void * get_qt_widget() override;
};

EXPORT MsrUiPlugin aud_plugin_instance;

static void on_window_enable_changed()
{
    PluginHandle * plugin = aud_plugin_by_header(&aud_plugin_instance);
    if (!plugin || !aud_plugin_get_enabled(plugin))
        return;

    hook_call("dock plugin disabled", plugin);
    if (aud_get_bool("monster-siren", "enable_dedicated_window"))
        hook_call("dock plugin enabled", plugin);
}

class CatalogFilterProxyModel : public QSortFilterProxyModel
{
public:
    explicit CatalogFilterProxyModel(QObject * parent = nullptr)
        : QSortFilterProxyModel(parent)
    {
    }

    void setFilterString(const QString & text)
    {
        m_filter_text = text.trimmed();
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        beginFilterChange();
        endFilterChange();
#else
        invalidateFilter();
#endif
    }

protected:
    bool filterAcceptsRow(int source_row, const QModelIndex & source_parent) const override
    {
        if (m_filter_text.isEmpty())
            return true;

        QModelIndex idx0 = sourceModel()->index(source_row, 0, source_parent);
        QModelIndex idx1 = sourceModel()->index(source_row, 1, source_parent);

        QString t0 = sourceModel()->data(idx0, Qt::DisplayRole).toString();
        QString t1 = sourceModel()->data(idx1, Qt::DisplayRole).toString();

        return t0.contains(m_filter_text, Qt::CaseInsensitive) || t1.contains(m_filter_text, Qt::CaseInsensitive);
    }

private:
    QString m_filter_text;
};

class SimpleListDelegate : public QStyledItemDelegate
{
public:
    explicit SimpleListDelegate(QObject * parent = nullptr) : QStyledItemDelegate(parent) {}

    void initStyleOption(QStyleOptionViewItem * option, const QModelIndex & index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        if (!aud_get_bool("monster-siren", "simple_mode_art")) {
            option->features &= ~QStyleOptionViewItem::HasDecoration;
            option->icon = QIcon();
        }
    }
};

class AlbumListDelegate : public QStyledItemDelegate
{
public:
    explicit AlbumListDelegate(QObject * parent = nullptr) : QStyledItemDelegate(parent) {}

    void paint(QPainter * painter, const QStyleOptionViewItem & option, const QModelIndex & index) const override
    {
        if (!index.isValid()) return;
        painter->save();

        if (option.state & QStyle::State_Selected) {
            painter->fillRect(option.rect, option.palette.highlight());
            painter->setPen(option.palette.highlightedText().color());
        } else {
            if (option.features & QStyleOptionViewItem::Alternate)
                painter->fillRect(option.rect, option.palette.alternateBase());
            else
                painter->fillRect(option.rect, option.palette.base());
            painter->setPen(option.palette.text().color());
        }

        QRect rect = option.rect;
        int icon_size = rect.height() - 8;
        if (icon_size < 32) icon_size = 32;

        QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
        QRect icon_rect(rect.left() + 4, rect.top() + (rect.height() - icon_size) / 2, icon_size, icon_size);
        if (!icon.isNull()) {
            icon.paint(painter, icon_rect);
        } else {
            painter->fillRect(icon_rect, Qt::darkGray);
        }

        QString title = index.data(Qt::DisplayRole).toString();
        QString artist = index.data(Qt::UserRole + 1).toString(); // custom role for artist

        QRect text_rect = rect.adjusted(icon_size + 12, 4, -4, -4);
        QFont title_font = option.font;
        title_font.setBold(true);
        painter->setFont(title_font);
        painter->drawText(text_rect.left(), text_rect.top() + 16, title);

        QFont artist_font = option.font;
        artist_font.setPointSize(artist_font.pointSize() - 1);
        painter->setFont(artist_font);
        if (!(option.state & QStyle::State_Selected))
            painter->setPen(option.palette.color(QPalette::Disabled, QPalette::Text));
        painter->drawText(text_rect.left(), text_rect.top() + 34, artist);

        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem & option, const QModelIndex & index) const override
    {
        (void)option; (void)index;
        return QSize(200, 56);
    }
};

class GridIconDelegate : public QStyledItemDelegate
{
public:
    explicit GridIconDelegate(QObject * parent = nullptr) : QStyledItemDelegate(parent) {}

    void paint(QPainter * painter, const QStyleOptionViewItem & option, const QModelIndex & index) const override
    {
        if (!index.isValid()) return;
        painter->save();

        if (option.state & QStyle::State_Selected) {
            painter->fillRect(option.rect, option.palette.highlight());
            painter->setPen(option.palette.highlightedText().color());
        } else {
            painter->setPen(option.palette.text().color());
        }

        QRect rect = option.rect.adjusted(4, 4, -4, -4);
        int art_size = rect.width();
        if (art_size > rect.height() - 36) art_size = rect.height() - 36;

        QRect art_rect(rect.left() + (rect.width() - art_size) / 2, rect.top(), art_size, art_size);
        QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
        if (!icon.isNull()) {
            icon.paint(painter, art_rect);
        } else {
            painter->fillRect(art_rect, Qt::darkGray);
        }

        QString title = index.data(Qt::DisplayRole).toString();
        QRect text_rect(rect.left(), art_rect.bottom() + 4, rect.width(), 32);
        QFont font = option.font;
        font.setPointSize(font.pointSize() - 1);
        painter->setFont(font);
        painter->drawText(text_rect, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, title);

        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem & option, const QModelIndex & index) const override
    {
        (void)option; (void)index;
        return QSize(140, 180);
    }
};

class MonsterSirenCatalogWidget : public QWidget
{
    Q_OBJECT

public:
    MonsterSirenCatalogWidget()
    {
        auto * layout = new QVBoxLayout(this);
        layout->setContentsMargins(4, 4, 4, 4);

        // Top control bar
        auto * top_bar = new QHBoxLayout();
        m_search_entry = new QLineEdit(this);
        m_search_entry->setPlaceholderText(QString::fromUtf8(_("Search Monster Siren albums or artists...")));
        m_search_entry->setClearButtonEnabled(true);

        m_view_switch_btn = new QPushButton(QString::fromUtf8(_("View: Simple")), this);
        m_refresh_btn = new QPushButton(QString::fromUtf8(_("Refresh")), this);

        top_bar->addWidget(m_search_entry, 1);
        top_bar->addWidget(m_view_switch_btn, 0);
        top_bar->addWidget(m_refresh_btn, 0);
        layout->addLayout(top_bar);

        // Model setup
        m_model = new QStandardItemModel(0, 2, this);
        m_model->setHeaderData(0, Qt::Horizontal, QString::fromUtf8(_("Album Title")));
        m_model->setHeaderData(1, Qt::Horizontal, QString::fromUtf8(_("Artistes")));

        m_proxy = new CatalogFilterProxyModel(this);
        m_proxy->setSourceModel(m_model);

        // Stacked widget for the 3 view modes
        m_stack = new QStackedWidget(this);

        // Mode 0: Simple List (QTreeView)
        m_tree = new QTreeView(m_stack);
        m_tree->setModel(m_proxy);
        m_tree->setItemDelegate(new SimpleListDelegate(m_tree));
        m_tree->setRootIsDecorated(false);
        m_tree->setAlternatingRowColors(true);
        m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
        m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_tree->header()->setStretchLastSection(true);
        m_tree->header()->resizeSection(0, 260);
        m_stack->addWidget(m_tree);

        // Mode 1: Album List (with cover art) (QListView with AlbumListDelegate)
        m_album_list = new QListView(m_stack);
        m_album_list->setModel(m_proxy);
        m_album_list->setItemDelegate(new AlbumListDelegate(m_album_list));
        m_album_list->setAlternatingRowColors(true);
        m_album_list->setSelectionMode(QAbstractItemView::SingleSelection);
        m_album_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_stack->addWidget(m_album_list);

        // Mode 2: Grid / Icon View (QListView in IconMode with GridIconDelegate)
        m_grid_view = new QListView(m_stack);
        m_grid_view->setModel(m_proxy);
        m_grid_view->setViewMode(QListView::IconMode);
        m_grid_view->setItemDelegate(new GridIconDelegate(m_grid_view));
        m_grid_view->setResizeMode(QListView::Adjust);
        m_grid_view->setGridSize(QSize(150, 190));
        m_grid_view->setWrapping(true);
        m_grid_view->setSpacing(6);
        m_grid_view->setSelectionMode(QAbstractItemView::SingleSelection);
        m_grid_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_stack->addWidget(m_grid_view);

        layout->addWidget(m_stack, 1);

        // Status label / hints
        m_status_label = new QLabel(QString::fromUtf8(_("Double-click an album to\nclear queue & play.")), this);
        m_status_label->setStyleSheet("color: gray; font-style: italic;");
        m_status_label->setWordWrap(true);
        layout->addWidget(m_status_label, 0);

        // Apply initial view mode from preferences
        int def_mode = aud_get_int("monster-siren", "default_view_mode");
        set_view_mode(def_mode);

        connect(m_search_entry, &QLineEdit::textChanged, this, [this](const QString & text) {
            m_proxy->setFilterString(text);
        });

        connect(m_view_switch_btn, &QPushButton::clicked, this, [this]() {
            int current = m_stack->currentIndex();
            int next = (current + 1) % 3;
            set_view_mode(next);
        });

        connect(m_refresh_btn, &QPushButton::clicked, this, [this]() {
            msr_cache_clear();
            refresh_catalog();
        });

        connect(m_tree, &QTreeView::doubleClicked, this, &MonsterSirenCatalogWidget::on_item_activated);
        connect(m_album_list, &QListView::doubleClicked, this, &MonsterSirenCatalogWidget::on_item_activated);
        connect(m_grid_view, &QListView::doubleClicked, this, &MonsterSirenCatalogWidget::on_item_activated);

        refresh_catalog();
    }

private:
    QLineEdit * m_search_entry = nullptr;
    QPushButton * m_view_switch_btn = nullptr;
    QPushButton * m_refresh_btn = nullptr;
    QStackedWidget * m_stack = nullptr;
    QTreeView * m_tree = nullptr;
    QListView * m_album_list = nullptr;
    QListView * m_grid_view = nullptr;
    QStandardItemModel * m_model = nullptr;
    CatalogFilterProxyModel * m_proxy = nullptr;
    QLabel * m_status_label = nullptr;
    std::atomic<bool> m_images_loaded{false};

    void set_view_mode(int mode)
    {
        if (mode < 0 || mode > 2) mode = 0;
        m_stack->setCurrentIndex(mode);
        if (mode == 0) m_view_switch_btn->setText(QString::fromUtf8(_("View: Simple")));
        else if (mode == 1) m_view_switch_btn->setText(QString::fromUtf8(_("View: Album List")));
        else m_view_switch_btn->setText(QString::fromUtf8(_("View: Grid")));

        if ((mode != 0 || aud_get_bool("monster-siren", "simple_mode_art")) && !m_images_loaded && m_model->rowCount() > 0) {
            start_async_image_loader();
        } else if (mode == 0 && m_tree) {
            m_tree->viewport()->update();
        }
    }

    void populate_catalog(const std::vector<MsrAlbum> & albums)
    {
        m_images_loaded = false;
        for (const auto & album : albums) {
            auto * item0 = new QStandardItem(QString::fromUtf8((const char *)album.name));
            auto * item1 = new QStandardItem(QString::fromUtf8((const char *)album.artistes));

            item0->setData(QString::fromUtf8((const char *)album.cid), Qt::UserRole);
            item0->setData(QString::fromUtf8((const char *)album.artistes), Qt::UserRole + 1);
            item0->setData(QString::fromUtf8((const char *)album.coverUrl), Qt::UserRole + 2);
            item1->setData(QString::fromUtf8((const char *)album.cid), Qt::UserRole);

            m_model->appendRow({item0, item1});
        }

        m_status_label->setText(QString::fromUtf8(_("Catalog loaded: %1 albums.\nDouble-click to clear queue & play."))
                                    .arg(albums.size()));
        if (m_refresh_btn)
            m_refresh_btn->setEnabled(true);

        int mode = m_stack->currentIndex();
        if (mode != 0 || aud_get_bool("monster-siren", "simple_mode_art")) {
            start_async_image_loader();
        }
    }

    void start_async_image_loader()
    {
        if (m_images_loaded.exchange(true))
            return;

        QPointer<MonsterSirenCatalogWidget> guard(this);
        std::thread([guard]() {
            if (!guard || !guard->m_model) return;
            int count = guard->m_model->rowCount();
            for (int i = 0; i < count; i++) {
                if (!guard) return;
                QStandardItem * item = guard->m_model->item(i, 0);
                if (!item) continue;
                QString cover_url = item->data(Qt::UserRole + 2).toString();
                if (cover_url.isEmpty()) continue;

                Index<char> img_bytes = msr_fetch_image(String(cover_url.toUtf8().constData()));
                if (!guard || !img_bytes.len()) continue;

                QByteArray qbytes(img_bytes.begin(), img_bytes.len());
                // Decode and scale down completely inside the worker thread (QImage is non-GUI and 100% thread-safe)
                QImage img;
                if (img.loadFromData(qbytes)) {
                    img = img.scaled(180, 180, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                }
                if (img.isNull()) continue;

                QMetaObject::invokeMethod(guard.data(), [guard, item, img]() {
                    if (!guard || !item) return;
                    item->setIcon(QIcon(QPixmap::fromImage(img)));
                }, Qt::QueuedConnection);
            }
        }).detach();
    }

    void refresh_catalog()
    {
        m_model->removeRows(0, m_model->rowCount());
        m_status_label->setText(QString::fromUtf8(_("Loading Monster Siren catalog...")));
        if (m_refresh_btn)
            m_refresh_btn->setEnabled(false);

        QPointer<MonsterSirenCatalogWidget> guard(this);
        std::thread([guard]() {
            std::vector<MsrAlbum> albums = msr_fetch_albums();
            if (!guard)
                return;
            QMetaObject::invokeMethod(guard.data(), [guard, albums = std::move(albums)]() {
                if (!guard)
                    return;
                guard->populate_catalog(albums);
            }, Qt::QueuedConnection);
        }).detach();
    }

    void on_item_activated(const QModelIndex & proxy_index)
    {
        if (!proxy_index.isValid())
            return;

        QModelIndex source_index = m_proxy->mapToSource(proxy_index);
        QString cid = m_model->data(source_index, Qt::UserRole).toString();
        if (cid.isEmpty())
            cid = m_model->data(source_index.siblingAtColumn(0), Qt::UserRole).toString();

        if (cid.isEmpty())
            return;

        AUDINFO("Selected Monster Siren album CID: %s -> clearing queue & starting playback\n", cid.toUtf8().constData());

        Playlist active = Playlist::active_playlist();
        active.remove_all_entries();

        Index<PlaylistAddItem> items;
        QString url = QString("msr://album/%1").arg(cid);
        items.append(String(url.toUtf8().constData()));

        active.insert_items(0, std::move(items), true);
    }
};

void * MsrUiPlugin::get_qt_widget()
{
    if (!aud_get_bool("monster-siren", "enable_dedicated_window"))
        return nullptr;

    return new MonsterSirenCatalogWidget();
}

#include "msr_ui.moc"
