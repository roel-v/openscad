#pragma once
#include "gui/Dock.h"
class QGLView;

class NavigationTuning : public Dock {
  Q_OBJECT
public:
  explicit NavigationTuning(QGLView *view);
  QString description() const;
private:
  QGLView *view;
};
