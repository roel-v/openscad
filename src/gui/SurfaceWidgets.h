#pragma once

#include <QAccessibilityHints>
#include <QBoxLayout>
#include <QChildEvent>
#include <QDockWidget>
#include <QGuiApplication>
#include <QLabel>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPointer>
#include <QStyleHints>
#include <QStylePainter>
#include <QStyleOptionTab>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>

namespace SurfaceWidgets {
inline bool highContrast()
{
  return QGuiApplication::styleHints()->accessibility()->contrastPreference() ==
         Qt::ContrastPreference::HighContrast;
}

inline QColor tint(const QPalette& palette, qreal amount)
{
  const QColor base = palette.color(QPalette::Window), accent = palette.color(QPalette::Accent);
  return QColor::fromRgbF(base.redF() + amount * (accent.redF() - base.redF()),
                          base.greenF() + amount * (accent.greenF() - base.greenF()),
                          base.blueF() + amount * (accent.blueF() - base.blueF()));
}

class DockTitle : public QWidget {
public:
  explicit DockTitle(QDockWidget *dock) : QWidget(dock), dock(dock)
  {
    setObjectName("scadmaxxDockTitle");
    layout = new QBoxLayout(QBoxLayout::LeftToRight, this);
    layout->setContentsMargins(6, 2, 4, 2);
    label = new QLabel(dock->windowTitle(), this);
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(label, 1);
    floating = new QToolButton(this);
    close = new QToolButton(this);
    for (auto *button : {floating, close}) {
      button->setAutoRaise(true);
      layout->addWidget(button);
    }
    connect(close, &QToolButton::clicked, dock, &QDockWidget::close);
    connect(floating, &QToolButton::clicked, dock, [dock] { dock->setFloating(!dock->isFloating()); });
    connect(dock, &QDockWidget::windowTitleChanged, label, &QLabel::setText);
    connect(dock, &QDockWidget::featuresChanged, this, [this] { refresh(); });
    connect(dock, &QDockWidget::topLevelChanged, this, [this] { refresh(); });
    connect(QGuiApplication::styleHints()->accessibility(),
            &QAccessibilityHints::contrastPreferenceChanged, this, [this] { update(); });
    refresh();
  }
protected:
  void paintEvent(QPaintEvent *) override
  {
    QPainter painter(this);
    painter.fillRect(rect(), highContrast() ? palette().color(QPalette::Window) : tint(palette(), 0.16));
  }
  void changeEvent(QEvent *event) override
  {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange ||
        event->type() == QEvent::FontChange) refresh();
  }
private:
  void refresh()
  {
    const auto features = dock->features();
    close->setVisible(features.testFlag(QDockWidget::DockWidgetClosable));
    floating->setVisible(features.testFlag(QDockWidget::DockWidgetFloatable));
    layout->setDirection(features.testFlag(QDockWidget::DockWidgetVerticalTitleBar) ?
                           QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    close->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton));
    floating->setIcon(style()->standardIcon(dock->isFloating() ? QStyle::SP_TitleBarNormalButton :
                                                                QStyle::SP_TitleBarMaxButton));
    close->setToolTip(tr("Close panel")); close->setAccessibleName(close->toolTip());
    floating->setToolTip(dock->isFloating() ? tr("Dock panel") : tr("Float panel"));
    floating->setAccessibleName(floating->toolTip());
    setAccessibleName(dock->windowTitle());
    updateGeometry(); update();
  }
  QDockWidget *dock;
  QBoxLayout *layout;
  QLabel *label;
  QToolButton *floating, *close;
};

// Also cover feature docks created after MainWindow::setupUi(). Never replace another custom title.
class DockInstaller : public QObject {
public:
  explicit DockInstaller(QMainWindow *window) : QObject(window), window(window)
  {
    window->installEventFilter(this);
    for (auto *dock : window->findChildren<QDockWidget *>()) install(dock);
  }
protected:
  bool eventFilter(QObject *object, QEvent *event) override
  {
    if (object == window && event->type() == QEvent::ChildAdded) {
      QPointer<QObject> child = static_cast<QChildEvent *>(event)->child();
      QTimer::singleShot(0, this, [this, child] {
        if (child) if (auto *dock = qobject_cast<QDockWidget *>(child.data())) install(dock);
      });
    }
    return false;
  }
private:
  static void install(QDockWidget *dock)
  {
    if (!dock->titleBarWidget()) dock->setTitleBarWidget(new DockTitle(dock));
  }
  QMainWindow *window;
};

class CategoryTabs : public QTabBar {
public:
  explicit CategoryTabs(QWidget *parent) : QTabBar(parent) { setMouseTracking(true); }
protected:
  void mouseMoveEvent(QMouseEvent *event) override
  {
    const int next = tabAt(event->position().toPoint());
    if (next != hovered) { hovered = next; update(); }
    QTabBar::mouseMoveEvent(event);
  }
  void leaveEvent(QEvent *event) override { hovered = -1; update(); QTabBar::leaveEvent(event); }
  void paintEvent(QPaintEvent *event) override
  {
    if (highContrast()) { QTabBar::paintEvent(event); return; }
    QStylePainter painter(this);
    for (int index = 0; index < count(); ++index) {
      QStyleOptionTab option; initStyleOption(&option, index);
      const bool selected = index == currentIndex();
      painter.fillRect(option.rect.adjusted(1, 1, -1, -1),
                       tint(option.palette, isTabEnabled(index) ? (selected ? 0.16 : index == hovered ? 0.08 : 0) : 0));
      painter.drawControl(QStyle::CE_TabBarTabLabel, option);
      if (selected) painter.fillRect(QRect(option.rect.left() + 2, option.rect.bottom() - 2,
                                           option.rect.width() - 4, 2), option.palette.color(QPalette::Accent));
      if (hasFocus() && selected) {
        QStyleOptionFocusRect focus; focus.initFrom(this); focus.rect = option.rect.adjusted(3, 3, -3, -3);
        painter.drawPrimitive(QStyle::PE_FrameFocusRect, focus);
      }
    }
  }
private:
  int hovered = -1;
};
} // namespace SurfaceWidgets
