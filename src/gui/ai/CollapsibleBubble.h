#pragma once

#include <QWidget>
#include <QVBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QFrame>
#include <QPalette>
#include <QApplication>
#include <QFontDatabase>
#include <vector>
#include <QString>

class CollapsibleBubble : public QWidget
{
  Q_OBJECT
public:
  CollapsibleBubble(const QString& summary, const QString& detail, QWidget *parent = nullptr)
    : QWidget(parent)
  {
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 4, 0, 4);

    QFrame *bubbleFrame = new QFrame(this);
    bubbleFrame->setFrameShape(QFrame::StyledPanel);

    bubbleFrame->setAutoFillBackground(true);
    bubbleFrame->setBackgroundRole(QPalette::AlternateBase);

    QVBoxLayout *frameLayout = new QVBoxLayout(bubbleFrame);
    frameLayout->setContentsMargins(8, 8, 8, 8);

    toggleButton = new QPushButton(this);
    toggleButton->setCheckable(true);
    toggleButton->setChecked(false);
    toggleButton->setFlat(true);
    QFont heading;
    heading.setWeight(QFont::Medium);
    toggleButton->setFont(heading);

    detailsLabel = new QLabel(this);
    detailsLabel->setWordWrap(true);
    detailsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont codeFont;
    codeFont.setFamily(QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
    detailsLabel->setFont(codeFont);
    detailsLabel->setForegroundRole(QPalette::Text);
    detailsLabel->hide();

    frameLayout->addWidget(toggleButton);
    frameLayout->addWidget(detailsLabel);
    layout->addWidget(bubbleFrame);

    addToolCall(summary, detail);

    connect(toggleButton, &QPushButton::clicked, this, [this](bool checked) {
      detailsLabel->setVisible(checked);
      updateButtonText();
    });
  }

  void addToolCall(const QString& summary, const QString& detail)
  {
    toolCalls.push_back({summary, detail});
    updateContent();
  }

private:
  bool isDarkTheme() const
  {
    QPalette pal = QApplication::palette();
    return pal.color(QPalette::Window).lightness() < 128;
  }

  void updateContent()
  {
    QString detailsText;
    for (size_t i = 0; i < toolCalls.size(); ++i) {
      if (i > 0) detailsText += "\n\n";
      detailsText += QString("■ %1\n%2").arg(toolCalls[i].summary, toolCalls[i].detail);
    }
    detailsLabel->setText(detailsText);
    updateButtonText();
  }

  void updateButtonText()
  {
    bool expanded = toggleButton->isChecked();
    QString arrow = expanded ? "▼" : "▶";
    QString text = QString("%1 AI used %2 tool(s)").arg(arrow).arg(toolCalls.size());
    toggleButton->setText(text);
  }

  struct ToolCallLog {
    QString summary;
    QString detail;
  };

  std::vector<ToolCallLog> toolCalls;
  QPushButton *toggleButton;
  QLabel *detailsLabel;
};
