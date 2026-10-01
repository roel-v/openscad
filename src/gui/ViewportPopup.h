#pragma once
#include <QFrame>
#include <QPointer>
#include <QListWidget>
#include <QPushButton>

class MainWindow;
class QRadioButton;
class QLabel;
class SavedViews;

class ViewportPopup : public QFrame {
  Q_OBJECT
public:
  explicit ViewportPopup(MainWindow *window);
  void toggle();
  void synchronize();
  QListWidget *views;
  QPushButton *addView, *removeView, *renameView;
  QLabel *documentLabel;
protected:
  void hideEvent(QHideEvent *) override;
  void keyPressEvent(QKeyEvent *) override;
private:
  MainWindow *window;
  QRadioButton *perspective, *orthographic;
  QPointer<QWidget> previousFocus;
  QPointer<SavedViews> store;
  void refreshViews();
  void recallView();
};
