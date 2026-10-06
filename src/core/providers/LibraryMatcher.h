// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/providers/ProviderTypes.h"
namespace Providers {
Result<ResolvedLibrary> matchLibrary(const LibraryRequirement& library, const LibraryProfile& profile,
  const Recipe& recipe, const LibraryResolutionContext& context);
Availability diagnosticAvailability(const Diagnostic& diagnostic);
}
