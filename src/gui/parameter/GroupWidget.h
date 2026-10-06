#pragma once

#include <QGridLayout>
#include <QString>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

class GroupWidget : public QWidget
{
  Q_OBJECT
private:
  QGridLayout mainLayout;
  QToolButton toggleButton;
  QWidget contentArea;
  QVBoxLayout contentLayout;
  const QString groupTitle;
  bool expanded = false;
  void updateSpacing();

protected:
  void changeEvent(QEvent *event) override;

public:
  GroupWidget(const QString& title, QWidget *parent = nullptr);
  void addWidget(QWidget *widget);

  bool isExpanded() const { return expanded; }
  QString title() const { return groupTitle; }

public slots:
  void setExpanded(bool expanded);
};
