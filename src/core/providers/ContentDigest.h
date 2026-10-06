// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/providers/ProviderTypes.h"
namespace Providers {
std::string sha256(const std::string& bytes);
Result<std::string> normalizedUtf8(const std::string& bytes, const std::string& path);
std::string closureDigest(const std::map<std::string, std::string>& hashes);
}
