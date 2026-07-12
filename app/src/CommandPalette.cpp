#include "CommandPalette.hpp"

#include <QAction>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>

CommandPalette::CommandPalette(const QList<QAction*>& actions, QWidget* parent)
    : QDialog(parent), actions_(actions) {
    setWindowTitle(QStringLiteral("Palette de commandes"));
    resize(480, 360);

    filterEdit_ = new QLineEdit(this);
    filterEdit_->setPlaceholderText(QStringLiteral("Tapez pour rechercher une commande..."));
    listWidget_ = new QListWidget(this);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(filterEdit_);
    layout->addWidget(listWidget_);

    connect(filterEdit_, &QLineEdit::textChanged, this, &CommandPalette::onFilterChanged);
    connect(filterEdit_, &QLineEdit::returnPressed, this, &CommandPalette::onActivateCurrentRow);
    connect(listWidget_, &QListWidget::itemActivated, this, &CommandPalette::onActivateCurrentRow);

    // Lets the user type to filter while still driving the list with the
    // arrow keys, without the QLineEdit consuming Up/Down for cursor
    // movement or Escape needing a separate button to close the dialog.
    filterEdit_->installEventFilter(this);

    rebuildList(QString());
    filterEdit_->setFocus();
}

bool CommandPalette::eventFilter(QObject* watched, QEvent* event) {
    if (watched == filterEdit_ && event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Down) {
            listWidget_->setCurrentRow(std::min(listWidget_->currentRow() + 1, listWidget_->count() - 1));
            return true;
        }
        if (keyEvent->key() == Qt::Key_Up) {
            listWidget_->setCurrentRow(std::max(listWidget_->currentRow() - 1, 0));
            return true;
        }
        if (keyEvent->key() == Qt::Key_Escape) {
            reject();
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void CommandPalette::onFilterChanged(const QString& text) {
    rebuildList(text);
}

void CommandPalette::rebuildList(const QString& filter) {
    listWidget_->clear();
    for (QAction* action : actions_) {
        QString text = action->text();
        text.remove(QLatin1Char('&'));
        if (filter.isEmpty() || text.contains(filter, Qt::CaseInsensitive)) {
            auto* item = new QListWidgetItem(text, listWidget_);
            item->setData(Qt::UserRole, QVariant::fromValue(static_cast<void*>(action)));
        }
    }
    if (listWidget_->count() > 0) listWidget_->setCurrentRow(0);
}

void CommandPalette::onActivateCurrentRow() {
    QListWidgetItem* item = listWidget_->currentItem();
    if (!item) return;
    auto* action = static_cast<QAction*>(item->data(Qt::UserRole).value<void*>());
    accept();
    if (action) action->trigger();
}
