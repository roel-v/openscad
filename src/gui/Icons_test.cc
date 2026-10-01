#include <catch2/catch_test_macros.hpp>
#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include "gui/Icons.h"
#include "gui/MessageBox.h"
#include "gui/OpenSCADApp.h"
#include "core/Settings.h"

void initializeGuiTestApplication();

namespace {
void iconApp()
{
  initializeGuiTestApplication();
  Icons::install();
}
struct RestoreAppearance {
  Settings::StatusIconAppearance saved = Settings::Settings::statusIconAppearance.value();
  ~RestoreAppearance() { Settings::Settings::statusIconAppearance.setValue(saved); }
};
}  // namespace

TEST_CASE("Icon mappings render all assets at logical sizes", "[ui-icons]")
{
  iconApp();
  RestoreAppearance restore;
  Settings::Settings::statusIconAppearance.set("monochrome");
  REQUIRE(Icons::names().size() >= 83);
  for (const auto& name : Icons::names()) {
    INFO(name.toStdString());
    const auto icon = Icons::icon(name);
    for (const auto dpr : {1.0, 1.25, 1.5, 2.0}) {
      const auto pixmap = Icons::pixmap(icon, QSize(24, 24), dpr);
      REQUIRE_FALSE(pixmap.isNull());
      const auto image = pixmap.toImage();
      bool hasInk = false;
      for (int y = 0; y < image.height() && !hasInk; ++y)
        for (int x = 0; x < image.width(); ++x)
          hasInk = hasInk || qAlpha(image.pixel(x, y)) > 0;
      CHECK(hasInk);
      CHECK(pixmap.width() == qRound(24 * dpr));
      CHECK(pixmap.devicePixelRatio() == dpr);
    }
  }
  CHECK(Icons::icon("not-an-icon").isNull());
}

TEST_CASE("Status choices default safely and refresh live pixmaps", "[ui-icons]")
{
  iconApp();
  RestoreAppearance restore;
  auto& setting = Settings::Settings::statusIconAppearance;
  setting.set("colored");
  setting.set("invalid-value");
  CHECK(Icons::appearance() == "native");
  QLabel first, second;
  Icons::setLabelIcon(&first, "information-icons-warning", 24);
  Icons::setLabelIcon(&second, "information-icons-warning", 24);
  const auto native = first.pixmap().toImage();
  setting.set("monochrome");
  Icons::refresh();
  CHECK(first.pixmap().toImage() != native);
  CHECK(first.pixmap().toImage() == second.pixmap().toImage());
  CHECK(setting.encode() == "monochrome");
  QPalette palette = first.palette();
  palette.setColor(QPalette::Active, QPalette::ButtonText, QColor(160, 20, 180));
  palette.setColor(QPalette::Inactive, QPalette::ButtonText, QColor(160, 20, 180));
  first.setPalette(palette);
  CHECK(first.pixmap().toImage() != second.pixmap().toImage());
  const auto enabled = Icons::icon("save", &first).pixmap(24, 24).toImage();
  CHECK(Icons::icon("save", &first).pixmap(24, 24, QIcon::Disabled).toImage() != enabled);
}

TEST_CASE("Message icon changes preserve modal buttons and results", "[ui-icons]")
{
  iconApp();
  RestoreAppearance restore;
  for (const auto mode : {"monochrome", "native", "colored"}) {
    Settings::Settings::statusIconAppearance.set(mode);
    MessageBox box(QMessageBox::Warning, "Unsaved changes", "Save the document?",
                   QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Save);
    box.setEscapeButton(QMessageBox::Cancel);
    QTimer::singleShot(0, &box, [&] {
      CHECK(box.defaultButton() == box.button(QMessageBox::Save));
      CHECK(box.escapeButton() == box.button(QMessageBox::Cancel));
      CHECK_FALSE(box.iconPixmap().isNull());
      Icons::refresh();
      box.button(QMessageBox::Cancel)->click();
    });
    CHECK(box.exec() == QMessageBox::Cancel);
  }
  MessageBox plain;
  Icons::prepareMessageBox(&plain);
  CHECK(plain.icon() == QMessageBox::NoIcon);
}
