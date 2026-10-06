#pragma once
#include "core/RenderVariables.h"
#include "core/customizer/ParameterSet.h"
#include <QPointer>
class EditorInterface;
// One ordinary compilation request, owned by MainWindow; no alternate live model.
struct EvaluationOverrides {
  RenderVariables variables;
  ParameterSet parameters;
  QPointer<EditorInterface> editor;
};
