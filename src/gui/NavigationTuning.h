#pragma once
#include <QDialog>
class QGLView;

class NavigationTuning : public QDialog {
  Q_OBJECT
public:
  explicit NavigationTuning(QGLView *view);
  QString description() const;
private:
  QGLView *view;
};
