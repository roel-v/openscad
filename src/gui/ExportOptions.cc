#include "gui/ExportOptions.h"
#include "gui/ExportPdfDialog.h"
#include "gui/Export3mfDialog.h"
bool configureGeometryExportOptions(FileFormat format, ExportInfo& info) {
  if (format == FileFormat::PDF) {
    ExportPdfDialog dialog; if (dialog.exec() == QDialog::Rejected) return false;
    info.optionsPdf = dialog.getOptions();
  } else if (format == FileFormat::_3MF) {
    Export3mfDialog dialog; if (dialog.exec() == QDialog::Rejected) return false;
    info.options3mf = dialog.getOptions();
  }
  return true;
}
