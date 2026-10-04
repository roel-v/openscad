#pragma once
#include "core/semantic/SemanticTargets.h"
#include "io/export.h"
#include <QString>
#include <functional>
namespace Semantic {
enum class ExportSpace { Local, AsPlaced };
struct StagedFile {
  QString path;
  ~StagedFile();
};
struct StagedExport {
  std::shared_ptr<const StagedFile> file;
  QString destination, error;
  bool valid() const { return file && error.isEmpty(); }
};
// Runs on a backend worker; creates only a unique sibling temporary file.
StagedExport stageExport(const PreparedTarget&, ExportSpace, const QString& destination,
                         const ExportInfo&, const std::function<bool()>& cancelled = {});
// Final caller revalidates the captured stamp/inputs immediately before atomic commit.
bool commitExport(const StagedExport&, const std::function<bool()>& current, QString *error = nullptr);
}
