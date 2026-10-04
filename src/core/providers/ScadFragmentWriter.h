// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/providers/ProviderTypes.h"
namespace Providers {
Result<GeneratedFragment> generateFragment(const ProviderBinding& binding, const ParameterValues& values,
                                           const SymbolBindings& names);
std::string scalarSource(const ScalarValue& value);
Result<LocalFrame> validateRecipeValues(const Recipe& recipe, const ParameterValues& values);
}
