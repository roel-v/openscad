#pragma once

#include <QGridLayout>
#include <QString>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

class GroupDisclosureButton : public QToolButton
{
public:
  explicit GroupDisclosureButton(QWidget *parent = nullptr);

protected:
  void paintEvent(QPaintEvent *event) override;
};

class GroupWidget : public QWidget
{
  Q_OBJECT
private:
  QGridLayout mainLayout;
  GroupDisclosureButton toggleButton;
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
  void updateVisibility();

  bool isExpanded() const { return expanded; }
  QString title() const { return groupTitle; }

public slots:
  void setExpanded(bool expanded);
};
