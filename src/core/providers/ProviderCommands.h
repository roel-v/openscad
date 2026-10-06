// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/providers/ProviderTypes.h"
#include <iosfwd>
namespace Providers {
struct ProviderDirectories {
  std::string resources, userConfig, sourceDirectory;
  std::vector<std::string> extraCatalogs;
};
struct DiscoveredProviders {
  CatalogSources catalogs;
  std::map<std::string,LibraryLocation> libraryBindings;
  std::vector<Diagnostic> diagnostics;
};
struct ProviderCommand {
  enum class Mode { List, Check, Emit } mode;
  std::string checkCatalog, qualifiedId, valuesPath, sourcePath;
  std::vector<std::string> extraCatalogs;
};
struct ProviderCommandEnvironment { std::string resources, userConfig, workingDirectory; };
DiscoveredProviders discoverProviders(const ProviderDirectories& directories);
int runProviderCommand(const ProviderCommand& command, const ProviderCommandEnvironment& environment, std::ostream& output);
int providerUsageError(const std::string& message,std::ostream& output);
}
