// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/insertion/InsertionTypes.h"
namespace Insertion {
// Native evaluation is process-local; production callers use the internal executable entry point.
PreviewResponse evaluatePreview(const PreviewRequest&);
int previewWorkerMain(int argc,char **argv);
}
