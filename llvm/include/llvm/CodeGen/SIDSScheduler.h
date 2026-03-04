// llvm/include/llvm/CodeGen/SIDSScheduler.h
#ifndef LLVM_CODEGEN_SIDSSCHEDULER_H
#define LLVM_CODEGEN_SIDSSCHEDULER_H

#include "llvm/CodeGen/MachineScheduler.h"
#include "llvm/CodeGen/ScheduleDAG.h"
#include "llvm/ADT/DenseMap.h"

namespace llvm {

class SIDSScheduler : public MachineSchedStrategy {
private:
  ScheduleDAGMI *DAG = nullptr;
  DenseMap<const SUnit *, float> StaticWeight;

  float Alpha = 0.4f;
  float Beta  = 0.4f;
  float Gamma = 0.2f;
  // Scale applied when an SUnit is a load/store to penalize potential misses.
  float MissPenaltyScale = 0.5f;
  // Adaptive miss-penalty support (simulation-based):
  bool AdaptiveMissPenalty = false;
  float AdaptiveBeta = 0.2f;   // smoothing/learning rate
  unsigned AdaptiveWindow = 32; // number of scheduled nodes per update window
  unsigned AdaptiveLoadCount = 0; // counter for loads scheduled in current window
  // Approximate \"instruction distance\" ℓ by tracking the order in which
  // SUnits are scheduled.
  DenseMap<const SUnit *, unsigned> ScheduledOrder;
  unsigned ScheduledCount = 0;
  // Improved control-distance support
  bool UseImprovedCtrl = false;
  // Cache for MBB distances computed on demand
  mutable llvm::DenseMap<const llvm::MachineBasicBlock *, llvm::DenseMap<const llvm::MachineBasicBlock *, unsigned>> MBBDistCache;

public:
  SIDSScheduler() = default;

  void initialize(ScheduleDAGMI *dag) override;
  SUnit *pickNode(bool &IsTopNode) override;
  float computeSIDSPriority(const SUnit *SU) const;

  void schedNode(SUnit *SU, bool IsTopNode) override;
  void releaseTopNode(SUnit *SU) override {}
  void releaseBottomNode(SUnit *SU) override {}
};

} // end namespace llvm

#endif