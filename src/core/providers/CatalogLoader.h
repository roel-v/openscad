// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/providers/ProviderTypes.h"
#include "ext/json/json.hpp"
namespace Providers {
Result<nlohmann::json> parseJson(const std::string& bytes, const std::string& path);
Result<ProviderDefinition> loadCatalog(const std::string& bytes, const std::string& path);
bool validIdentifier(const std::string& value);
bool validRelativePath(const std::string& value, bool allowEmpty = false);
Result<ParameterValues> validateValues(const ItemDefinition& item, const ParameterValues& values);
}
