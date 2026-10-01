#pragma once

#include <QIcon>
#include <QStringList>

class QWidget;
class QLabel;
class QMessageBox;

namespace Icons {
// Stable preference values: monochrome, native (default), colored.
QString appearance();
QStringList names();
QIcon icon(const QString& name, const QWidget *context = nullptr);
QPixmap pixmap(const QIcon& icon, const QSize& size, qreal dpr, QIcon::Mode mode = QIcon::Normal,
               QIcon::State state = QIcon::Off);
void install();
void refresh();
void setLabelIcon(QLabel *label, const QString& name, int size, qreal rotation = 0);
void prepareMessageBox(QMessageBox *box);
}  // namespace Icons
