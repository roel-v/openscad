#include "../../src/gui/annotations/AnnotationStore.h"
#include "../../src/gui/annotations/AnnotationDrawing.h"
#include <QBuffer>
#include <QDateTime>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <QJsonDocument>
#include <zip.h>
using namespace AgentAnnotations;

static Record example()
{
  QImage image(30, 20, QImage::Format_RGB32);
  image.fill(Qt::red);
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  image.save(&buffer, "PNG");
  QJsonObject camera{{"translation", QJsonArray{0, 0, 0}},
                     {"rotation", QJsonArray{0, 0, 0}},
                     {"distance", 100},
                     {"fov", 45},
                     {"projection", "perspective"}};
  return {{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
           {"created_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
           {"comment", QString::fromUtf8("Unicode: \xe2\x98\x83")},
           {"width", 30},
           {"height", 20},
           {"png_sha256", QString::fromLatin1(Store::hash(bytes))},
           {"camera", camera},
           {"strokes", QJsonArray{}},
           {"result", QJsonObject{}}},
          bytes};
}
class StoreTest : public QObject
{
  Q_OBJECT
private slots:
  void drawingHistoryAndCoordinates()
  {
    Drawing drawing;
    const QSize size(200, 100);
    drawing.begin({20, 20}, size, Qt::red);
    drawing.end(size);
    QVERIFY(drawing.strokes.isEmpty());
    drawing.begin({20, 20}, size, Qt::red);
    drawing.move({120, 60}, size);
    drawing.end(size);
    QCOMPARE(drawing.strokes.size(), 1);
    QCOMPARE(drawing.json()[0].toObject().value("points").toArray()[1].toArray(),
             QJsonArray({0.6, 0.6}));
    drawing.begin({40, 40}, size, Qt::blue);
    drawing.move({220, 40}, size);
    drawing.end(size);
    QCOMPARE(drawing.strokes.size(), 1);
    drawing.undo();
    QVERIFY(!drawing.marked());
    QCOMPARE(drawing.redoStrokes.size(), 1);
    drawing.redo();
    QCOMPARE(drawing.strokes.size(), 1);
    drawing.undo();
    drawing.navigation();
    drawing.redo();
    QVERIFY(drawing.strokes.isEmpty());
    drawing.begin({20, 20}, size, Qt::green);
    drawing.move({160, 60}, size);
    drawing.end(size);
    QImage image(400, 200, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    {
      QPainter painter(&image);
      drawing.paint(painter, image.size(), 2);
    }
    QVERIFY(qAlpha(image.pixel(40, 40)) > 0);
    QCOMPARE(qAlpha(image.pixel(20, 20)), 0);
  }
  void roundTripAndCopy()
  {
    QTemporaryDir dir;
    QString path = dir.filePath("model.scad.annotations"), error;
    Store store;
    auto record = example();
    QVERIFY2(store.add(record, &error), qPrintable(error));
    QVERIFY(store.dirty());
    QVERIFY2(store.save(path, true, &error), qPrintable(error));
    QVERIFY(!store.dirty());
    Store loaded;
    QVERIFY2(loaded.load(path, &error), qPrintable(error));
    QCOMPARE(loaded.records().size(), 1);
    QCOMPARE(loaded.records()[0].png, record.png);
    QCOMPARE(loaded.records()[0].metadata, record.metadata);
    QVERIFY(loaded.sessionId != store.sessionId);
    QVERIFY(store.edit(record.id(), "new comment"));
    QVERIFY(store.save(dir.filePath("copy.scad.annotations"), false, &error));
    QVERIFY(store.dirty());
    QCOMPARE(loaded.records()[0].metadata.value("comment"), record.metadata.value("comment"));
  }
  void historyAndReloadBoundary()
  {
    Store store;
    QSignalSpy commands(&store, &Store::commandAdded);
    auto record = example();
    QVERIFY(store.add(record));
    int add = commands.last()[0].toInt();
    QVERIFY(store.edit(record.id(), "edit"));
    int edit = commands.last()[0].toInt();
    QVERIFY(store.remove(record.id()));
    int remove = commands.last()[0].toInt();
    store.applyHistory(remove, true);
    QCOMPARE(store.records()[0].png, record.png);
    store.applyHistory(edit, true);
    QCOMPARE(store.records()[0].metadata, record.metadata);
    store.applyHistory(add, true);
    QVERIFY(!store.dirty());
    store.applyHistory(add, false);
    store.clearHistory();
    store.applyHistory(add, true);
    QCOMPARE(store.records().size(), 1);
    QVERIFY(store.dirty());
  }
  void conflictsAndCorruption()
  {
    QTemporaryDir dir;
    QString path = dir.filePath("model.scad.annotations"), error;
    Store store;
    QVERIFY(store.load(path));
    QVERIFY(store.add(example()));
    QVERIFY(store.save(path, true));
    Store second;
    QVERIFY(second.load(path));
    QVERIFY(store.edit(store.records()[0].id(), "first writer"));
    QVERIFY(store.save(path, true));
    QVERIFY(second.remove(second.records()[0].id()));
    QVERIFY(!second.save(path, true, &error));
    QVERIFY(error.contains("changed on disk"));
    QVERIFY(second.dirty());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("not an archive");
    file.close();
    Store bad;
    QVERIFY(!bad.load(path, &error));
    QVERIFY(!bad.add(example(), &error));
    QVERIFY(!bad.save(path, true, &error));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("not an archive"));
  }
  void emptyAndDestination()
  {
    QTemporaryDir dir;
    QString path = dir.filePath("a.scad.annotations"), error;
    Store store;
    QVERIFY(store.save(path, true));
    QVERIFY(!QFile::exists(path));
    auto record = example();
    QVERIFY(store.add(record));
    QVERIFY(store.save(path, true));
    Store empty;
    QVERIFY(!empty.save(path, false, &error));
    QVERIFY(empty.save(path, false, &error, true));
    Store reopened;
    QVERIFY(reopened.load(path));
    QVERIFY(reopened.records().isEmpty());
  }
  void validationAndFailures()
  {
    auto record = example();
    QString error;
    QVERIFY(Store::validate(record));
    record.metadata["width"] = 1000000;
    QVERIFY(!Store::validate(record, &error));
    record = example();
    record.png[0] = 'x';
    QVERIFY(!Store::validate(record, &error));
    record = example();
    record.metadata["camera"] = QJsonObject{};
    QVERIFY(!Store::validate(record, &error));
    Store store;
    QVERIFY(store.add(example()));
    QTemporaryDir dir;
    QVERIFY(!store.save(dir.filePath("missing/a.annotations"), true, &error));
    QVERIFY(store.dirty());
  }
  void unsupportedManifestAndUnexpectedEntry()
  {
    QTemporaryDir dir;
    QString path = dir.filePath("unsupported.annotations"), error;
    Store store;
    auto record = example();
    QVERIFY(store.add(record));
    QVERIFY(store.save(path, true));
    int ze = 0;
    auto archive = zip_open(path.toUtf8().constData(), 0, &ze);
    QVERIFY(archive);
    QByteArray manifest = QJsonDocument(QJsonObject{{"schema", "openscad.annotations"},
                                                    {"version", 999},
                                                    {"records", QJsonArray{record.metadata}}})
                            .toJson();
    auto entry = zip_source_buffer(archive, manifest.constData(), manifest.size(), 0);
    QVERIFY(zip_file_add(archive, "manifest.json", entry, ZIP_FL_OVERWRITE) >= 0);
    QCOMPARE(zip_close(archive), 0);
    Store loaded;
    QVERIFY(!loaded.load(path, &error));
    QVERIFY(error.contains("Unsupported"));
    QVERIFY(!loaded.save(path, true));
    archive = zip_open(path.toUtf8().constData(), 0, &ze);
    QVERIFY(archive);
    manifest = QJsonDocument(QJsonObject{{"schema", "openscad.annotations"},
                                         {"version", 1},
                                         {"records", QJsonArray{record.metadata}}})
                 .toJson();
    entry = zip_source_buffer(archive, manifest.constData(), manifest.size(), 0);
    QVERIFY(zip_file_add(archive, "manifest.json", entry, ZIP_FL_OVERWRITE) >= 0);
    entry = zip_source_buffer(archive, "x", 1, 0);
    QVERIFY(zip_file_add(archive, "../escape", entry, 0) >= 0);
    QCOMPARE(zip_close(archive), 0);
    QVERIFY(!loaded.load(path, &error));
    QVERIFY(error.contains("Unexpected"));
  }
  void compressedExpansionLimit()
  {
    QTemporaryDir dir;
    const auto path = dir.filePath("expansion.annotations");
    int ze = 0;
    auto archive = zip_open(path.toUtf8().constData(), ZIP_CREATE | ZIP_TRUNCATE, &ze);
    QVERIFY(archive);
    const QByteArray manifest(8 * 1024 * 1024 + 1, ' ');
    auto entry = zip_source_buffer(archive, manifest.constData(), manifest.size(), 0);
    const auto index = zip_file_add(archive, "manifest.json", entry, 0);
    QVERIFY(index >= 0);
    QCOMPARE(zip_set_file_compression(archive, index, ZIP_CM_DEFLATE, 9), 0);
    QCOMPARE(zip_close(archive), 0);
    QVERIFY(QFileInfo(path).size() < 100000);
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto original = file.readAll();
    file.close();
    Store store;
    QString error;
    QVERIFY(!store.load(path, &error));
    QVERIFY(error.contains("manifest"));
    QVERIFY(!store.save(path, true));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), original);
  }
};
QTEST_MAIN(StoreTest)
#include "StoreTest.moc"
