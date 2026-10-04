#include "gui/source/DiskSourceState.h"
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryDir>
#include <catch2/catch_all.hpp>

using namespace SourceEditing;
namespace {
void put(const QString& path, const QByteArray& bytes)
{
  QFile file(path);
  REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  REQUIRE(file.write(bytes) == bytes.size());
}
QByteArray get(const QString& path)
{
  QFile file(path);
  REQUIRE(file.open(QIODevice::ReadOnly));
  return file.readAll();
}
}

TEST_CASE("Disk source snapshots detect equal-length edits with unchanged timestamps", "[source-save-conflicts]")
{
  QTemporaryDir dir;
  const auto path = dir.filePath(QString::fromUtf8("\xc3\xa9-\xf0\x9f\x98\x80.scad"));
  put(path, "cube(10);\r\n");
  const auto baseline = DiskSourceState::capture(path);
  const auto timestamp = QFileInfo(path).lastModified();
  put(path, "cube(20);\r\n");
  QFile changed(path);
  REQUIRE(changed.open(QIODevice::ReadWrite));
  REQUIRE(changed.setFileTime(timestamp, QFileDevice::FileModificationTime));
  changed.close();
  REQUIRE_FALSE(baseline.matches(DiskSourceState::capture(path)));
  auto failed = writeSource(path, "unsaved buffer", baseline);
  REQUIRE_FALSE(failed.saved);
  REQUIRE(failed.conflict);
  REQUIRE(get(path) == "cube(20);\r\n");
  REQUIRE(QFile::remove(path));
  REQUIRE_FALSE(baseline.matches(DiskSourceState::capture(path)));
  put(path, "cube(30);\r\n");
  REQUIRE_FALSE(baseline.matches(DiskSourceState::capture(path)));
  REQUIRE_FALSE(baseline.matches(DiskSourceState::capture(dir.filePath("other.scad"))));
  REQUIRE(DiskSourceState::capture(dir.path()).status == DiskReadStatus::Unreadable);
  const auto current = DiskSourceState::capture(path);
  QSaveFile replacement(path);
  REQUIRE(replacement.open(QIODevice::WriteOnly));
  REQUIRE(replacement.write("cube(30);\r\n") == 11);
  REQUIRE(replacement.commit());
  const auto replaced = DiskSourceState::capture(path);
  REQUIRE(current.sha256 == replaced.sha256);
  REQUIRE_FALSE(current.matches(replaced));
}

TEST_CASE("Atomic source writes recheck approval and leave original intact on failure", "[source-save-conflicts]")
{
  QTemporaryDir dir;
  const auto path = dir.filePath("source.scad");
  put(path, "original");
  const auto baseline = DiskSourceState::capture(path);
  SECTION("injected write failure") {
    const auto result = writeSource(path, "new", baseline, [](QString& error) { error = "injected I/O failure"; return false; });
    REQUIRE_FALSE(result.saved);
    REQUIRE(result.error == "injected I/O failure");
    REQUIRE(get(path) == "original");
  }
  SECTION("approved version changes before commit") {
    const auto result = writeSource(path, "new", baseline, [&](QString&) { put(path, "third-party"); return true; });
    REQUIRE_FALSE(result.saved);
    REQUIRE(result.conflict);
    REQUIRE(get(path) == "third-party");
  }
  SECTION("successful save establishes exact committed-byte baseline") {
    const auto result = writeSource(path, "new\r\n", baseline);
    REQUIRE(result.saved);
    REQUIRE(result.state.matches(DiskSourceState::capture(path)));
    REQUIRE(get(path) == "new\r\n");
  }
  SECTION("new destination is guarded too") {
    const auto destination = dir.filePath("copy.scad");
    const auto approved = DiskSourceState::capture(destination);
    put(destination, "new owner");
    REQUIRE(writeSource(destination, "copy", approved).conflict);
    REQUIRE(get(destination) == "new owner");
    REQUIRE(get(path) == "original");
  }
}
