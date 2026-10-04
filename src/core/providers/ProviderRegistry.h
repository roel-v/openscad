// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/providers/ProviderTypes.h"
namespace Providers {
class ProviderRegistry {
public:
  static std::shared_ptr<const RegistrySnapshot> load(const CatalogSources& sources);
  static std::vector<ItemStatus> query(std::shared_ptr<const RegistrySnapshot> snapshot,
                                      const LibraryResolutionContext& context);
  static Result<ProviderBinding> bind(std::shared_ptr<const RegistrySnapshot> snapshot,
                                      const std::string& qualifiedId, const LibraryResolutionContext& context);
  static Result<ProviderBinding> revalidate(const ProviderBinding& binding, const LibraryResolutionContext& context,
                                           std::shared_ptr<const RegistrySnapshot> currentSnapshot);
  static Result<GeneratedFragment> generate(const ProviderBinding& binding, const ParameterValues& values,
                                           const SymbolBindings& names);
};
}
