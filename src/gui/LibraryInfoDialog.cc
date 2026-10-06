
#include "gui/LibraryInfoDialog.h"

#include <QString>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextBlockFormat>
#include <QTextEdit>

#include "LibraryInfo.h"

LibraryInfoDialog::LibraryInfoDialog(const QString& rendererInfo)
{
  setupUi(this);
  updateLibraryInfo(rendererInfo);
}

void LibraryInfoDialog::on_okButton_clicked()
{
  accept();
}

void LibraryInfoDialog::updateLibraryInfo(const QString& rendererInfo)
{
  // Get library infos
  QString info(LibraryInfo::info().c_str());
  info += rendererInfo;

  // Keep copied text literal, with modest label emphasis and native-font row spacing.
  infoTextBox->setPlainText(info);
  for (auto block = infoTextBox->document()->begin(); block.isValid(); block = block.next()) {
    QTextCursor cursor(block);
    QTextBlockFormat spacing;
    spacing.setBottomMargin(infoTextBox->fontMetrics().height() / 6.0);
    cursor.setBlockFormat(spacing);
    const int colon = block.text().indexOf(':');
    if (colon >= 0) {
      cursor.setPosition(block.position() + colon + 1, QTextCursor::KeepAnchor);
      QTextCharFormat emphasis;
      emphasis.setFontWeight(QFont::Medium);
      cursor.mergeCharFormat(emphasis);
    }
  }
}
