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
    nullptr
};

static void on_window_enable_changed();

static const PreferencesWidget msr_ui_widgets[] = {
    WidgetCheck(N_("Enable dockable catalog window"),
                WidgetBool("monster-siren", "enable_dedicated_window", on_window_enable_changed))
};

static const PluginPreferences msr_ui_prefs = {.widgets = msr_ui_widgets};

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
        setFilterCaseSensitivity(Qt::CaseInsensitive);
    }

protected:
    bool filterAcceptsRow(int source_row, const QModelIndex & source_parent) const override
    {
        QModelIndex idx0 = sourceModel()->index(source_row, 0, source_parent);
        QModelIndex idx1 = sourceModel()->index(source_row, 1, source_parent);

        QString query = filterRegularExpression().pattern();
        if (query.isEmpty())
            return true;

        QString t0 = sourceModel()->data(idx0, Qt::DisplayRole).toString();
        QString t1 = sourceModel()->data(idx1, Qt::DisplayRole).toString();

        return t0.contains(query, Qt::CaseInsensitive) || t1.contains(query, Qt::CaseInsensitive);
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

        m_refresh_btn = new QPushButton(QString::fromUtf8(_("Refresh")), this);

        top_bar->addWidget(m_search_entry, 1);
        top_bar->addWidget(m_refresh_btn, 0);
        layout->addLayout(top_bar);

        // Tree view & model setup
        m_model = new QStandardItemModel(0, 2, this);
        m_model->setHeaderData(0, Qt::Horizontal, QString::fromUtf8(_("Album Title")));
        m_model->setHeaderData(1, Qt::Horizontal, QString::fromUtf8(_("Artistes")));

        m_proxy = new CatalogFilterProxyModel(this);
        m_proxy->setSourceModel(m_model);

        m_tree = new QTreeView(this);
        m_tree->setModel(m_proxy);
        m_tree->setRootIsDecorated(false);
        m_tree->setAlternatingRowColors(true);
        m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
        m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_tree->header()->setStretchLastSection(true);
        m_tree->header()->resizeSection(0, 260);

        layout->addWidget(m_tree, 1);

        // Status label / hints
        m_status_label = new QLabel(QString::fromUtf8(_("Double-click an album to\nclear queue & play.")), this);
        m_status_label->setStyleSheet("color: gray; font-style: italic;");
        m_status_label->setWordWrap(true);
        layout->addWidget(m_status_label, 0);

        connect(m_search_entry, &QLineEdit::textChanged, this, [this](const QString & text) {
            m_proxy->setFilterRegularExpression(QRegularExpression::escape(text));
        });

        connect(m_refresh_btn, &QPushButton::clicked, this, [this]() {
            msr_cache_clear();
            refresh_catalog();
        });

        connect(m_tree, &QTreeView::doubleClicked, this, &MonsterSirenCatalogWidget::on_item_activated);

        refresh_catalog();
    }

private:
    QLineEdit * m_search_entry = nullptr;
    QPushButton * m_refresh_btn = nullptr;
    QTreeView * m_tree = nullptr;
    QStandardItemModel * m_model = nullptr;
    CatalogFilterProxyModel * m_proxy = nullptr;
    QLabel * m_status_label = nullptr;

    void populate_catalog(const std::vector<MsrAlbum> & albums)
    {
        for (const auto & album : albums) {
            auto * item0 = new QStandardItem(QString::fromUtf8((const char *)album.name));
            auto * item1 = new QStandardItem(QString::fromUtf8((const char *)album.artistes));

            item0->setData(QString::fromUtf8((const char *)album.cid), Qt::UserRole);
            item1->setData(QString::fromUtf8((const char *)album.cid), Qt::UserRole);

            m_model->appendRow({item0, item1});
        }

        m_status_label->setText(QString::fromUtf8(_("Catalog loaded: %1 albums.\nDouble-click to clear queue & play."))
                                    .arg(albums.size()));
        if (m_refresh_btn)
            m_refresh_btn->setEnabled(true);
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
