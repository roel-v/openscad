// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/insertion/SourceInventory.h"
#include "core/providers/ProviderRegistry.h"
namespace Insertion {
struct PlacementDraft;
struct PlannedPatch {ByteEdit edit;std::string expected;};
struct EditPlan {
  SourceTarget target;
  std::vector<PlannedPatch> patches;
  std::string candidate,candidateHash,itemModule;
  Providers::ProviderBinding provider;
  std::vector<GeneratedBinding> expectedBindings;
  std::vector<VolumeCheck> subtractTargets;
  std::vector<std::string> affectedPaths;
  bool sharedApproved=false;
};
class InsertionPlanner {
public:
  static Result<EditPlan> geometry(const SourceInventory&,const SourceTarget&,
    const Providers::GeneratedFragment&,const Transform3d& placement,bool sharedApproved);
  static Result<EditPlan> component(const SourceInventory&,const SourceTarget&,
    const Providers::GeneratedFragment&,const PlacementDraft&,bool sharedApproved);
  static Result<std::vector<PlannedPatch>> coalesce(const SourceInput&,std::vector<PlannedPatch>);
  static Result<std::string> apply(const SourceInput&,const std::vector<PlannedPatch>&);
  static Result<std::string> imports(const SourceInventory&,const std::string&,
    const Providers::GeneratedFragment&,uint64_t prefixPoint);
  static std::string transformSource(const Transform3d&);
  static PreviewRequest preview(const EditPlan&,uint64_t serial,const std::string& build,
    const std::string& directory,const NativeConfiguration&,const EvaluationInputs&);
};
}
