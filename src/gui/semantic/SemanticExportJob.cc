#include "gui/semantic/SemanticExportJob.h"
#include "core/NativeExecutionGuard.h"
#include "geometry/Geometry.h"
#ifdef ENABLE_MANIFOLD
#include "geometry/manifold/ManifoldGeometry.h"
#endif
#ifdef ENABLE_CGAL
#include "geometry/cgal/CGALNefGeometry.h"
#endif
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryFile>
namespace Semantic {
StagedFile::~StagedFile() { if (!path.isEmpty()) QFile::remove(path); }
StagedExport stageExport(const PreparedTarget& target, ExportSpace space, const QString& destination,
                         const ExportInfo& info, const std::function<bool()>& cancelled) {
  StagedExport output; output.destination = QFileInfo(destination).absoluteFilePath();
  auto stopped = [&] { return cancelled && cancelled(); };
  if (stopped()) { output.error = "Export cancelled"; return output; }
  if (target.state != Design::ResolutionState::Resolved) { output.error = QString::fromStdString(target.diagnostic); return output; }
  auto geometry = space == ExportSpace::Local ? target.localGeometry : target.placedGeometry;
  const bool supported = info.format == FileFormat::ASCII_STL || info.format == FileFormat::BINARY_STL ||
    info.format == FileFormat::OBJ || info.format == FileFormat::OFF || info.format == FileFormat::WRL ||
    info.format == FileFormat::_3MF || info.format == FileFormat::DXF || info.format == FileFormat::SVG || info.format == FileFormat::PDF;
  NativeExecutionGuard execution;
  if (!supported || !geometry || geometry->isEmpty() ||
      (fileformat::is3D(info.format) ? geometry->getDimension() != 3 : geometry->getDimension() != 2)) {
    output.error = "Target is empty or incompatible with the selected geometry format"; return output;
  }
#ifdef ENABLE_MANIFOLD
  if (auto manifold = dynamic_cast<const ManifoldGeometry *>(geometry.get()); manifold && !manifold->isValid()) { output.error = "Target geometry is not a valid manifold"; return output; }
#endif
#ifdef ENABLE_CGAL
  if (auto nef = dynamic_cast<const CGALNefGeometry *>(geometry.get()); nef && !nef->p3->is_simple()) { output.error = "Target geometry is not a simple manifold"; return output; }
#endif
  const QFileInfo requested(output.destination);
  QTemporaryFile temporary(requested.dir().filePath(".scadmaxx-target-XXXXXX." + QString::fromStdString(fileformat::toSuffix(info.format))));
  if (!temporary.open()) { output.error = temporary.errorString(); return output; }
  auto staged = std::make_shared<StagedFile>(); staged->path = temporary.fileName(); temporary.setAutoRemove(false); temporary.close();
  try {
    if (stopped() || !exportFileByName(geometry, staged->path.toStdString(), info) || QFileInfo(staged->path).size() == 0) { output.error = stopped() ? "Export cancelled" : "Target exporter failed"; return output; }
  } catch (const std::exception& error) { output.error = QString::fromUtf8(error.what()); return output; }
  if (stopped()) { output.error = "Export cancelled"; return output; }
  output.file = std::move(staged); return output;
}
bool commitExport(const StagedExport& staged, const std::function<bool()>& current, QString *error) {
  auto fail = [&](const QString& message) { if (error) *error = message; return false; };
  if (!staged.valid()) return fail(staged.error.isEmpty() ? "Export output is unavailable" : staged.error);
  if (!current || !current()) return fail("Export inputs or target changed");
  QFile input(staged.file->path); if (!input.open(QIODevice::ReadOnly)) return fail(input.errorString());
  QSaveFile output(staged.destination); output.setDirectWriteFallback(false);
  if (!output.open(QIODevice::WriteOnly)) return fail(output.errorString());
  while (!input.atEnd()) {
    const auto bytes = input.read(1024 * 1024);
    if ((bytes.isEmpty() && input.error() != QFileDevice::NoError) || output.write(bytes) != bytes.size()) { output.cancelWriting(); return fail("Cannot copy staged target output"); }
  }
  if (!current()) { output.cancelWriting(); return fail("Export cancelled or inputs changed before commit"); }
  if (!output.commit()) return fail(output.errorString()); return true;
}
}
