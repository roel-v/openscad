#pragma once
#include "core/anchors/ReferenceScene.h"
namespace Anchors {
Design::DecodeResult decodeReference(const std::string& kind, const Value&, const Design::DeclarationContext&);
void registerKinds();
}
