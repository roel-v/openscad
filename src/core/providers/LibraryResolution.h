// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/providers/ProviderTypes.h"
namespace Providers {
Result<LibraryResolutionContext> nativeResolutionContext(const std::string& sourceDirectory,
  const std::map<std::string,LibraryLocation>& bindings = {});
std::string resolutionContextDigest(const LibraryResolutionContext& context);
}
