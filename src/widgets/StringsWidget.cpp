
#include <QModelIndex>

#include "StringsWidget.h"
#include "ui_StringsWidget.h"

#include "MainWindow.h"
#include "common/Helpers.h"
#include "dialogs/XrefsDialog.h"

#include "WidgetShortcuts.h"

#include <QMenu>
#include <QClipboard>

StringsModel::StringsModel(QList<StringDescription> *strings, QObject *parent)
    : QAbstractListModel(parent),
      strings(strings)
{
}

int StringsModel::rowCount(const QModelIndex &) const
{
    return strings->count();
}

int StringsModel::columnCount(const QModelIndex &) const
{
    return Columns::COUNT;
}

QVariant StringsModel::data(const QModelIndex &index, int role) const
{
    if (index.row() >= strings->count())
        return QVariant();

    const StringDescription &str = strings->at(index.row());

    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case OFFSET:
            return RAddressString(str.vaddr);
        case STRING:
            return str.string;
        case TYPE:
            return str.type.toUpper();
        case LENGTH:
            return str.length;
        case SIZE:
            return str.size;
        default:
            return QVariant();
        }
    case StringDescriptionRole:
        return QVariant::fromValue(str);
    default:
        return QVariant();
    }
}

QVariant StringsModel::headerData(int section, Qt::Orientation, int role) const
{
    switch (role) {
    case Qt::DisplayRole:
        switch (section) {
        case OFFSET:
            return tr("Address");
        case STRING:
            return tr("String");
        case TYPE:
            return tr("Type");
        case LENGTH:
            return tr("Length");
        case SIZE:
            return tr("Size");
        default:
            return QVariant();
        }
    default:
        return QVariant();
    }
}

StringsSortFilterProxyModel::StringsSortFilterProxyModel(StringsModel *source_model,
                                                         QObject *parent)
    : QSortFilterProxyModel(parent)
{
    setSourceModel(source_model);
    setFilterCaseSensitivity(Qt::CaseInsensitive);
    setSortCaseSensitivity(Qt::CaseInsensitive);
}

bool StringsSortFilterProxyModel::filterAcceptsRow(int row, const QModelIndex &parent) const
{
    QModelIndex index = sourceModel()->index(row, 0, parent);
    StringDescription str = index.data(StringsModel::StringDescriptionRole).value<StringDescription>();
    return str.string.contains(filterRegExp());
}

bool StringsSortFilterProxyModel::lessThan(const QModelIndex &left, const QModelIndex &right) const
{
    StringDescription left_str = left.data(
                                     StringsModel::StringDescriptionRole).value<StringDescription>();
    StringDescription right_str = right.data(
                                      StringsModel::StringDescriptionRole).value<StringDescription>();

    switch (left.column()) {
    case StringsModel::OFFSET:
        return left_str.vaddr < right_str.vaddr;
    case StringsModel::STRING: // sort by string
        return left_str.string < right_str.string;
    case StringsModel::TYPE: // sort by type
        return left_str.type < right_str.type;
    case StringsModel::SIZE: // sort by size
        return left_str.size < right_str.size;
    case StringsModel::LENGTH: // sort by length
        return left_str.length < right_str.length;
    default:
        break;
    }

    // fallback
    return left_str.vaddr < right_str.vaddr;
}


StringsWidget::StringsWidget(MainWindow *main, QAction *action) :
    CutterDockWidget(main, action),
    ui(new Ui::StringsWidget),
    tree(new CutterTreeWidget(this))
{
    ui->setupUi(this);

    // Add Status Bar footer
    tree->addStatusBar(ui->verticalLayout);

    qhelpers::setVerticalScrollMode(ui->stringsTreeView);

    // Shift-F12 to toggle strings window
    QShortcut *toggle_shortcut = new QShortcut(widgetShortcuts["StringsWidget"], main);
    connect(toggle_shortcut, &QShortcut::activated, this, [=] (){ 
            toggleDockWidget(true);
            main->updateDockActionChecked(action);
            } );

    // Ctrl-F to show/hide the filter entry
    QShortcut *search_shortcut = new QShortcut(QKeySequence::Find, this);
    connect(search_shortcut, &QShortcut::activated, ui->quickFilterView, &QuickFilterView::showFilter);
    search_shortcut->setContext(Qt::WidgetWithChildrenShortcut);

    // Esc to clear the filter entry
    QShortcut *clear_shortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(clear_shortcut, &QShortcut::activated, ui->quickFilterView, &QuickFilterView::clearFilter);
    clear_shortcut->setContext(Qt::WidgetWithChildrenShortcut);

    connect(ui->actionFilter, SIGNAL(triggered()), ui->quickFilterView, SLOT(showFilter()));
    connect(ui->actionCopy_String, SIGNAL(triggered()), this, SLOT(on_actionCopy()));
    connect(ui->actionCopy_Address, SIGNAL(triggered()), this, SLOT(on_actionCopy()));

    connect(ui->stringsTreeView, SIGNAL(customContextMenuRequested(const QPoint &)),
            this, SLOT(showStringsContextMenu(const QPoint &)));

    ui->actionFilter->setShortcut(QKeySequence::Find);

    ui->stringsTreeView->setContextMenuPolicy(Qt::CustomContextMenu);

    model = new StringsModel(&strings, this);
    proxy_model = new StringsSortFilterProxyModel(model, this);
    ui->stringsTreeView->setModel(proxy_model);
    ui->stringsTreeView->sortByColumn(StringsModel::OFFSET, Qt::AscendingOrder);

    connect(ui->quickFilterView, SIGNAL(filterTextChanged(const QString &)), proxy_model,
            SLOT(setFilterWildcard(const QString &)));
    connect(ui->quickFilterView, SIGNAL(filterClosed()), ui->stringsTreeView, SLOT(setFocus()));

    connect(ui->quickFilterView, &QuickFilterView::filterTextChanged, this, [this] {
        tree->showItemsNumber(proxy_model->rowCount());
    });
    
    connect(Core(), SIGNAL(refreshAll()), this, SLOT(refreshStrings()));
}

StringsWidget::~StringsWidget() {}

void StringsWidget::on_stringsTreeView_doubleClicked(const QModelIndex &index)
{
    if (!index.isValid()) {
        return;
    }

    StringDescription str = index.data(StringsModel::StringDescriptionRole).value<StringDescription>();
    Core()->seek(str.vaddr);
}

void StringsWidget::refreshStrings()
{
    if (task) {
        task->wait();
    }

    task = QSharedPointer<StringsTask>(new StringsTask());
    connect(task.data(), &StringsTask::stringSearchFinished, this,
            &StringsWidget::stringSearchFinished);
    Core()->getAsyncTaskManager()->start(task);
}

void StringsWidget::stringSearchFinished(const QList<StringDescription> &strings)
{
    model->beginResetModel();
    this->strings = strings;
    model->endResetModel();

    qhelpers::adjustColumns(ui->stringsTreeView, 5, 0);
    if (ui->stringsTreeView->columnWidth(1) > 300)
        ui->stringsTreeView->setColumnWidth(1, 300);

    tree->showItemsNumber(proxy_model->rowCount());

    task = nullptr;
}

void StringsWidget::showStringsContextMenu(const QPoint &pt)
{
    QMenu *menu = new QMenu(ui->stringsTreeView);

    menu->clear();
    menu->addAction(ui->actionCopy_String);
    menu->addAction(ui->actionCopy_Address);
    menu->addAction(ui->actionFilter);
    menu->addSeparator();
    menu->addAction(ui->actionX_refs);

    menu->exec(ui->stringsTreeView->mapToGlobal(pt));

    delete menu;
}

void StringsWidget::on_actionX_refs_triggered()
{
    StringDescription str = ui->stringsTreeView->selectionModel()->currentIndex().data(
                StringsModel::StringDescriptionRole).value<StringDescription>();

    XrefsDialog *x = new XrefsDialog(this);
    x->fillRefsForAddress(str.vaddr, RAddressString(str.vaddr), false);
    x->setAttribute(Qt::WA_DeleteOnClose);
    x->exec();
}

void StringsWidget::on_actionCopy()
{
    QModelIndex current_item = ui->stringsTreeView->currentIndex();
    int row = current_item.row();

    QModelIndex index;

    if (sender() == ui->actionCopy_String) {
        index = ui->stringsTreeView->model()->index(row, 1);
    } else if (sender() == ui->actionCopy_Address) {
        index = ui->stringsTreeView->model()->index(row, 0);
    }

    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setText(index.data().toString());
}
