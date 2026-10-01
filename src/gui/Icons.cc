#include "gui/Icons.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QFile>
#include <QHash>
#include <QIconEngine>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QComboBox>
#include <QListWidget>
#include <QToolBar>
#include <QMessageBox>
#include <QPainter>
#include <QPixmapCache>
#include <QPointer>
#include <QStyle>
#include <QSvgRenderer>
#include <QTimer>
#include <QWidget>
#include <cmath>
#include "core/Settings.h"

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

static void initIconResources()
{
  Q_INIT_RESOURCE(icons_tabler);
}

namespace {
QString canonical(QString name)
{
  if (name.startsWith("chokusen-")) name.remove(0, 9);
  if (name.startsWith("openscad-")) name.remove(0, 9);
  return name;
}

const QJsonObject& mappings()
{
  static const auto entries = [] {
    initIconResources();
    QFile file(":/icons/tabler/mapping.json");
    file.open(QIODevice::ReadOnly);
    return QJsonDocument::fromJson(file.readAll()).object();
  }();
  return entries;
}

int statusSymbol(const QString& name)
{
  if (name == "information-icons-warning" || name == "emblem-unreadable")
    return QStyle::SP_MessageBoxWarning;
  if (name == "information-icons-error" || name == "circle-error") return QStyle::SP_MessageBoxCritical;
  if (name == "information-icons-info") return QStyle::SP_MessageBoxInformation;
  return -1;
}

bool highContrast()
{
#ifdef Q_OS_WIN
  HIGHCONTRAST info = {};
  info.cbSize = sizeof(info);
  return SystemParametersInfo(SPI_GETHIGHCONTRAST, sizeof(info), &info, 0) &&
         (info.dwFlags & HCF_HIGHCONTRASTON);
#else
  return false;
#endif
}

QColor nativeTint(const QIcon& native, const QColor& fallback)
{
  // Sample the actual style artwork: no guessed warning/error/info palette roles or RGB constants.
  const auto image = native.pixmap(48, 48).toImage().convertToFormat(QImage::Format_ARGB32);
  QHash<QRgb, int> counts;
  QRgb best = fallback.rgba();
  int maximum = 0;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const auto pixel = image.pixel(x, y);
      if (qAlpha(pixel) < 240 || QColor(pixel).hsvSaturation() < 80) continue;
      const auto count = ++counts[pixel];
      if (count > maximum) {
        maximum = count;
        best = pixel;
      }
    }
  }
  return QColor::fromRgba(best);
}

double luminance(QColor c)
{
  auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
  return .2126 * linear(c.redF()) + .7152 * linear(c.greenF()) + .0722 * linear(c.blueF());
}
double contrastRatio(QColor a, QColor b)
{
  const double first = luminance(a), second = luminance(b);
  return (std::max(first, second) + .05) / (std::min(first, second) + .05);
}
QColor readableTint(QColor color, const QColor& background)
{
  const double bg = luminance(background);
  for (int i = 0; i < 32; ++i) {
    const double fg = luminance(color);
    if ((std::max(bg, fg) + .05) / (std::min(bg, fg) + .05) >= 3) break;
    color = bg > .5 ? color.darker(110) : color.lighter(110);
  }
  return color;
}

class IconEngine : public QIconEngine
{
public:
  IconEngine(QString name, const QWidget *context) : name(std::move(name)), context(context) {}
  QIconEngine *clone() const override { return new IconEngine(name, context); }
  QString key() const override { return "OpenSCADPaletteIcon"; }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  QString iconName() override { return "openscad-" + name; }
#else
  QString iconName() const override { return "openscad-" + name; }
  void virtual_hook(int id, void *data) override
  {
    if (id == QIconEngine::ScaledPixmapHook) {
      auto *args = static_cast<QIconEngine::ScaledPixmapArgument *>(data);
      args->pixmap =
        render((QSizeF(args->size) / args->scale).toSize(), args->mode, args->state, args->scale);
    } else QIconEngine::virtual_hook(id, data);
  }
#endif
  QSize actualSize(const QSize& size, QIcon::Mode, QIcon::State) override { return size; }
  QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
  {
    return render(size, mode, state, 1);
  }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override
  {
#if QT_VERSION < QT_VERSION_CHECK(6, 8, 0)
    return render((QSizeF(size) / scale).toSize(), mode, state, scale);
#else
    return render(size, mode, state, scale);
#endif
  }
#endif
  QPixmap render(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale)
  {
    auto *widget = context.data();
    auto *style = widget ? widget->style() : QApplication::style();
    const auto palette = widget ? widget->palette() : QApplication::palette();
    const auto group = mode == QIcon::Disabled
                         ? QPalette::Disabled
                         : (widget && !widget->isActiveWindow() ? QPalette::Inactive : QPalette::Active);
    // Windows11 gives checked tool buttons an accent background. Other styles keep ButtonText.
    const bool checkedAccent = state == QIcon::On && mode != QIcon::Disabled &&
                               (qobject_cast<const QAbstractButton *>(widget) ||
                                qobject_cast<const QToolBar *>(widget)) &&
                               style->objectName().compare("windows11", Qt::CaseInsensitive) == 0;
    const bool selected = mode == QIcon::Selected || checkedAccent;
    const auto role =
      widget && !qobject_cast<const QAbstractButton *>(widget) && !qobject_cast<const QToolBar *>(widget)
        ? widget->foregroundRole()
        : QPalette::ButtonText;
    auto foreground = palette.color(group, selected ? QPalette::HighlightedText : role);
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    if (checkedAccent) {
      const auto background = palette.color(group, QPalette::Accent);
      // HighlightedText belongs to Highlight, not necessarily to the checked-button Accent fill.
      if (contrastRatio(foreground, background) < 4.5) {
        for (const auto role :
             {QPalette::WindowText, QPalette::Light, QPalette::Dark, QPalette::Shadow}) {
          const auto candidate = palette.color(group, role);
          if (contrastRatio(candidate, background) > contrastRatio(foreground, background))
            foreground = candidate;
        }
      }
    }
#endif
    const bool contrast = highContrast();
    const auto treatment = Icons::appearance();
    const auto status = statusSymbol(name);
    const auto native =
      status < 0 ? QIcon()
                 : style->standardIcon(static_cast<QStyle::StandardPixmap>(status), nullptr, widget);
    if (status >= 0 && treatment == "native") return Icons::pixmap(native, size, scale, mode, state);
    auto color = foreground;
    if (status >= 0 && treatment == "colored" && !contrast && mode != QIcon::Disabled && !selected)
      color = readableTint(nativeTint(native, foreground), palette.color(group, QPalette::Window));
    const auto cacheKey = QString("scad-icon/%1/%2/%3/%4/%5/%6/%7/%8")
                            .arg(name)
                            .arg(size.width())
                            .arg(size.height())
                            .arg(scale, 0, 'g', 12)
                            .arg(color.rgba())
                            .arg(palette.cacheKey())
                            .arg(reinterpret_cast<quintptr>(style))
                            .arg(treatment);
    QPixmap result;
    if (QPixmapCache::find(cacheKey, &result)) return result;
    QFile file(":/icons/tabler/" + mappings().value(name).toString());
    if (!file.open(QIODevice::ReadOnly)) return {};
    auto svg = file.readAll();
    svg.replace("currentColor", color.name().toUtf8());
    QSvgRenderer renderer(svg);
    result = QPixmap((QSizeF(size) * scale).toSize());
    result.setDevicePixelRatio(scale);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    painter.setOpacity(color.alphaF());
    renderer.render(&painter, QRectF(QPointF(), QSizeF(size)));
    painter.end();
    QPixmapCache::insert(cacheKey, result);
    return result;
  }
  void paint(QPainter *painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override
  {
    painter->drawPixmap(rect, render(rect.size(), mode, state, painter->device()->devicePixelRatioF()));
  }

private:
  QString name;
  QPointer<const QWidget> context;
};

void updateLabel(QLabel *label)
{
  const auto name = label->property("openscadIcon").toString();
  if (name.isEmpty()) return;
  const int size = label->property("openscadIconSize").toInt();
  const auto dpr = label->devicePixelRatioF();
  QPixmap output(qRound(size * dpr), qRound(size * dpr));
  output.setDevicePixelRatio(dpr);
  output.fill(Qt::transparent);
  QPainter painter(&output);
  painter.translate(size / 2.0, size / 2.0);
  painter.rotate(label->property("openscadIconRotation").toReal());
  painter.translate(-size / 2.0, -size / 2.0);
  Icons::icon(name, label)
    .paint(&painter, QRect(0, 0, size, size), Qt::AlignCenter,
           label->isEnabled() ? QIcon::Normal : QIcon::Disabled);
  painter.end();
  label->setPixmap(output);
}

class IconEvents : public QObject
{
public:
  using QObject::QObject;
  bool eventFilter(QObject *object, QEvent *event) override
  {
    if (busy) return false;
    if (event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::ThemeChange ||
        event->type() == QEvent::StyleChange) {
      if (!pending) {
        pending = true;
        QTimer::singleShot(0, this, [this] {
          pending = false;
          Icons::refresh();
        });
      }
    }
    if (event->type() == QEvent::Polish || event->type() == QEvent::Show ||
        event->type() == QEvent::ActionAdded ||
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
        event->type() == QEvent::DevicePixelRatioChange ||
#endif
        event->type() == QEvent::ActionChanged || event->type() == QEvent::PaletteChange ||
        event->type() == QEvent::EnabledChange || event->type() == QEvent::WindowActivate ||
        event->type() == QEvent::WindowDeactivate || event->type() == QEvent::ScreenChangeInternal) {
      if (auto *widget = qobject_cast<QWidget *>(object)) update(widget);
    }
    return false;
  }
  void update(QWidget *widget)
  {
    if (busy) return;
    busy = true;
    auto convert = [widget](const QIcon& old) {
      const auto name = canonical(old.name());
      return mappings().contains(name) ? Icons::icon(name, widget) : old;
    };
    for (auto *action : widget->actions()) {
      if (mappings().contains(canonical(action->icon().name())))
        action->setIcon(convert(action->icon()));
    }
    if (auto *button = qobject_cast<QAbstractButton *>(widget)) {
      if (mappings().contains(canonical(button->icon().name())))
        button->setIcon(convert(button->icon()));
    }
    if (auto *label = qobject_cast<QLabel *>(widget)) updateLabel(label);
    if (auto *combo = qobject_cast<QComboBox *>(widget)) {
      for (int i = 0; i < combo->count(); ++i)
        if (mappings().contains(canonical(combo->itemIcon(i).name())))
          combo->setItemIcon(i, convert(combo->itemIcon(i)));
    }
    if (auto *list = qobject_cast<QListWidget *>(widget)) {
      for (int i = 0; i < list->count(); ++i)
        if (mappings().contains(canonical(list->item(i)->icon().name())))
          list->item(i)->setIcon(convert(list->item(i)->icon()));
    }
    if (auto *box = qobject_cast<QMessageBox *>(widget)) {
      if (box->property("openscadMessageIcon").isValid()) Icons::prepareMessageBox(box);
    }
    widget->update();
    busy = false;
  }

private:
  bool busy = false;
  bool pending = false;
};
QPointer<IconEvents> events;
}  // namespace

namespace Icons {
QString appearance()
{
  const auto value = QString::fromStdString(Settings::Settings::statusIconAppearance.encode());
  return value == "monochrome" || value == "colored" ? value : "native";
}
QStringList names()
{
  return mappings().keys();
}
QIcon icon(const QString& input, const QWidget *context)
{
  const auto name = canonical(input);
  return mappings().contains(name) ? QIcon(new IconEngine(name, context)) : QIcon();
}
QPixmap pixmap(const QIcon& icon, const QSize& size, qreal dpr, QIcon::Mode mode, QIcon::State state)
{
  QPixmap result((QSizeF(size) * dpr).toSize());
  result.setDevicePixelRatio(dpr);
  result.fill(Qt::transparent);
  QPainter painter(&result);
  icon.paint(&painter, QRect(QPoint(), size), Qt::AlignCenter, mode, state);
  return result;
}
void install()
{
  if (!events) {
    events = new IconEvents(qApp);
    qApp->installEventFilter(events);
  }
}
void refresh()
{
  install();
  for (auto *widget : QApplication::allWidgets()) events->update(widget);
}
void setLabelIcon(QLabel *label, const QString& name, int size, qreal rotation)
{
  label->setProperty("openscadIcon", name);
  label->setProperty("openscadIconSize", size);
  label->setProperty("openscadIconRotation", rotation);
  updateLabel(label);
}
void prepareMessageBox(QMessageBox *box)
{
  install();
  const auto saved = box->property("openscadMessageIcon");
  const int symbol = saved.isValid() ? saved.toInt() : int(box->icon());
  if (symbol == QMessageBox::NoIcon) return;
  box->setProperty("openscadMessageIcon", symbol);
  // Use Qt's standard dialog widgets in every mode so a live setting change can update its icon.
  // This does not change the platform style or window chrome, or affect OS-owned file dialogs.
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
  box->setOption(QMessageBox::Option::DontUseNativeDialog);
#endif
  QString name;
  switch (symbol) {
  case QMessageBox::Warning:     name = "information-icons-warning"; break;
  case QMessageBox::Critical:    name = "information-icons-error"; break;
  case QMessageBox::Information: name = "information-icons-info"; break;
  case QMessageBox::Question:    name = "information-icons-question"; break;
  default:                       return;
  }
  const int size = box->style()->pixelMetric(QStyle::PM_MessageBoxIconSize, nullptr, box);
  box->setIconPixmap(pixmap(icon(name, box), QSize(size, size), box->devicePixelRatioF()));
}
}  // namespace Icons
