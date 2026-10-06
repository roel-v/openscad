// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/insertion/InsertionTypes.h"
#include <QByteArray>
#include <QCborMap>
class Context;
namespace Insertion {
NativeConfiguration captureConfiguration(const Context&,ParameterSet);
QCborMap encodeConfiguration(const NativeConfiguration&);
NativeConfiguration decodeConfiguration(const QCborValue&);
std::string configurationHash(const NativeConfiguration&);
std::string executableIdentity();
std::vector<std::string> compiledFeatures();
QCborMap handshake();
void validateHandshake(const QCborValue&,const std::string& expectedBuild);
QCborMap encodeRequest(const PreviewRequest&);
PreviewRequest decodeRequest(const QCborValue&);
QCborMap encodeResponse(const PreviewResponse&);
PreviewResponse decodeResponse(const QCborValue&);
void validateProof(const PreviewRequest&,const PreviewResponse&);
QByteArray frame(const QCborValue&);
// Returns nullopt until the complete frame arrives; rejects a second/trailing frame.
std::optional<QCborValue> takeFrame(QByteArray&);
}
