// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/providers/ContentDigest.h"
#include <QCryptographicHash>
#include <QString>
namespace Providers {
std::string sha256(const std::string& bytes)
{
  return QCryptographicHash::hash(QByteArray::fromStdString(bytes), QCryptographicHash::Sha256).toHex().toStdString();
}
Result<std::string> normalizedUtf8(const std::string& bytes, const std::string& path)
{
  // An ASCII prefix prevents Qt from consuming an initial BOM during validation.
  const auto encoded=QByteArray("x")+QByteArray::fromStdString(bytes);
  if (QString::fromUtf8(encoded).toUtf8()!=encoded)
    return {{}, Diagnostic{"InvalidUtf8", path, "", "Source is not valid UTF-8"}};
  std::string normalized;
  size_t i = bytes.compare(0, 3, "\xef\xbb\xbf") == 0 ? 3 : 0;
  for (; i < bytes.size(); ++i) {
    if (bytes[i] == '\r') {
      normalized += '\n';
      if (i + 1 < bytes.size() && bytes[i + 1] == '\n') ++i;
    } else normalized += bytes[i];
  }
  return {std::move(normalized), {}};
}
std::string closureDigest(const std::map<std::string, std::string>& hashes)
{
  std::string bytes;
  for (const auto& [path, hash] : hashes) { bytes += path; bytes += '\0'; bytes += hash; bytes += '\n'; }
  return sha256(bytes);
}
}
