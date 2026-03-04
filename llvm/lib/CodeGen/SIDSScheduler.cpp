// // llvm/lib/CodeGen/SIDSScheduler.cpp
// #include "llvm/CodeGen/SIDSScheduler.h"
// #include "llvm/CodeGen/MachineInstr.h"
// #include "llvm/Support/Debug.h"
// #include "llvm/Support/raw_ostream.h"

// #define DEBUG_TYPE "sids-scheduler"

// using namespace llvm;

// void SIDSScheduler::initialize(ScheduleDAGMI *dag) {
//   DAG = dag;

//   // 示例权重初始化（后续替换为您的静态分析结果）
//   for (SUnit &SU : DAG->SUnits) {
//     float reuse = 1.0f;
//     float hot   = 1.0f;
//     float ctrl  = static_cast<float>(SU.NodeNum) / DAG->SUnits.size();

//     float weight = Alpha * reuse + Beta * hot - Gamma * ctrl;
//     StaticWeight[&SU] = weight;
//   }

//   //LLVM_DEBUG(dbgs() << "SIDS Scheduler initialized. Total SUnits: "
//                     << DAG->SUnits.size() << "\n");
// }

// float SIDSScheduler::computeSIDSPriority(const SUnit *SU) const {
//   float priority = 0.0f;

//   for (const SDep &PredDep : SU->Preds) {
//     if (PredDep.getKind() == SDep::Data) {
//       const SUnit *Pred = PredDep.getSUnit();
//       auto It = StaticWeight.find(Pred);
//       if (It != StaticWeight.end()) {
//         priority = std::max(priority, It->second);
//       }
//     }
//   }

//   return priority;
// }

// SUnit *SIDSScheduler::pickNode(bool &IsTopNode) {
//   SUnit *Best = nullptr;
//   float BestPriority = -1.0f;
//   IsTopNode = true;  // 默认使用 Top 方向调度

//   // 当前版本中没有直接的 isReady()，改为检查是否在 Top/Bottom 就绪状态
//   // 同时排除已调度节点
//   for (SUnit &SU : DAG->SUnits) {
//     // 跳过已调度或非就绪节点
//     if (SU.isScheduled || (!SU.isTopReady() && !SU.isBottomReady())) {
//       continue;
//     }

//     float Prio = computeSIDSPriority(&SU);
//     if (Prio > BestPriority) {
//       BestPriority = Prio;
//       Best = &SU;
//       // 根据实际就绪队列方向设置 IsTopNode
//       IsTopNode = SU.isTopReady();
//     }
//   }

//   if (Best) {
//     LLVM_DEBUG({
//       dbgs() << "SIDS picked: SU#" << Best->NodeNum << " (";
//       if (Best->getInstr()) {
//         Best->getInstr()->print(dbgs(), /*IsStandalone=*/true);
//       } else {
//         dbgs() << "Entry/Exit";
//       }
//       dbgs() << ") Priority: " << BestPriority << "\n";
//     });
//   }

//   return Best;
// }



// llvm/lib/CodeGen/SIDSScheduler.cpp
#include "llvm/CodeGen/SIDSScheduler.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/CodeGen/MachineScheduler.h"

#define DEBUG_TYPE "sids-scheduler"

using namespace llvm;

// === 可通过命令行调整的参数（强烈推荐）===
static cl::opt<float> SIDSAlpha("sids-alpha", cl::desc("SIDS Reuse weight"), cl::init(0.4));
static cl::opt<float> SIDSBeta ("sids-beta",  cl::desc("SIDS Hot weight"),   cl::init(0.4));
static cl::opt<float> SIDSGamma("sids-gamma", cl::desc("SIDS Ctrl penalty"), cl::init(0.2));
static cl::opt<float> SIDSMissPenalty("sids-miss-penalty",
                                     cl::desc("SIDS Miss penalty scale"),
                                     cl::init(0.5f));
static cl::opt<bool> SIDSMissPenaltyAdaptive("sids-miss-penalty-adaptive",
                                             cl::desc("Enable adaptive (simulated) miss penalty"),
                                             cl::init(false));
static cl::opt<float> SIDSMissPenaltyBeta("sids-miss-penalty-beta",
                                          cl::desc("Adaptive miss-penalty smoothing factor (0..1)"),
                                          cl::init(0.2f));
static cl::opt<unsigned> SIDSMissPenaltyWindow("sids-miss-penalty-window",
                                               cl::desc("Adaptive miss-penalty update window (scheduled nodes)"),
                                               cl::init(32));
static cl::opt<bool> SIDSUseImprovedCtrl("sids-ctrl-improved",
                                         cl::desc("Use improved control-distance metric (CFG-based)"),
                                         cl::init(false));

// Register a MachineSchedRegistry entry so -mllvm -misched=sids works.
static ScheduleDAGInstrs *createSIDSched(MachineSchedContext *C) {
  return new ScheduleDAGMI(C, std::make_unique<SIDSScheduler>(), true);
}
static MachineSchedRegistry SIDSRegistry("sids", "SIDS scheduler",
                                        createSIDSched);

void SIDSScheduler::initialize(ScheduleDAGMI *dag) {
  DAG = dag;

  Alpha = SIDSAlpha;
  Beta  = SIDSBeta;
  Gamma = SIDSGamma;
  MissPenaltyScale = SIDSMissPenalty;
  // Initialize adaptive parameters from command line
  AdaptiveMissPenalty = SIDSMissPenaltyAdaptive;
  AdaptiveBeta = SIDSMissPenaltyBeta;
  AdaptiveWindow = SIDSMissPenaltyWindow;
  AdaptiveLoadCount = 0;
  ScheduledOrder.clear();
  ScheduledCount = 0;
  // improved ctrl
  UseImprovedCtrl = SIDSUseImprovedCtrl;
  if (UseImprovedCtrl)
    MBBDistCache.clear();

  // Compute a more meaningful static weight per SUnit using a lightweight
  // approximation of:
  //  - 数据复用度 Reuse: 1 / (平均“定义->使用”逻辑距离 + 1)
  //  - 热点 Hot: 节点越靠近关键路径（深度越大）越“热”
  //  - 控制距离 Ctrl: 节点号在 DAG 中越靠后，视为控制距离越大
  for (SUnit &SU : DAG->SUnits) {
    // ---- Reuse: 近似“定义->使用”平均距离 ----
    unsigned SumDist = 0;
    unsigned DistCount = 0;
    for (const SDep &SuccDep : SU.Succs) {
      if (SuccDep.getKind() == SDep::Data)
        if (const SUnit *Succ = SuccDep.getSUnit()) {
          unsigned DefDepth = SU.getDepth();
          unsigned UseDepth = Succ->getDepth();
          if (UseDepth >= DefDepth) {
            SumDist += (UseDepth - DefDepth);
            ++DistCount;
          }
        }
    }
    // 平均“指令距离”
    float AvgDist =
        DistCount ? static_cast<float>(SumDist) / static_cast<float>(DistCount)
                  : 0.0f;
    // 复用度 = 1 / (平均距离 + 1)，范围 (0, 1]
    float reuse = 1.0f / (AvgDist + 1.0f);

    // ---- Hot: 使用节点深度作为关键路径的粗略代理 ----
    float hot = 1.0f + static_cast<float>(SU.getDepth()) * 0.1f;

    // ---- Ctrl: 使用 NodeNum 归一化作为控制距离的近似 ----
    float ctrl = static_cast<float>(SU.NodeNum) / std::max<size_t>(1, DAG->SUnits.size());

    // If improved ctrl requested, we compute a placeholder now; full values
    // may be adjusted later using MBB distances when needed.
    if (SIDSUseImprovedCtrl)
      ctrl = ctrl; // keep for now; compute on-demand in computeSIDSPriority

    float weight = Alpha * reuse + Beta * hot - Gamma * ctrl;
    StaticWeight[&SU] = weight;
  }

  LLVM_DEBUG(dbgs() << "=== SIDS Scheduler initialized ===\n"
                    << "  Alpha=" << Alpha << " Beta=" << Beta << " Gamma=" << Gamma << " MissPenalty=" << MissPenaltyScale << "\n"
                    << "  Total SUnits: " << DAG->SUnits.size() << "\n");
}

// (No free helper; MBB distance is computed inline in computeSIDSPriority so
// the mutable MBBDistCache can be updated from const method.)

float SIDSScheduler::computeSIDSPriority(const SUnit *SU) const {
  float priority = 0.0f;
  // Current tentative position in the final schedule if we picked SU next.
  unsigned CurIndex = ScheduledCount;

  // 继承前驱最大权重（数据依赖），并估算 w·ℓ 形式的距离代价
  float DistanceCost = 0.0f;
  for (const SDep &PredDep : SU->Preds) {
    if (PredDep.getKind() == SDep::Data) {
      const SUnit *Pred = PredDep.getSUnit();
      auto It = StaticWeight.find(Pred);
      if (It != StaticWeight.end()) {
        priority = std::max(priority, It->second);
      }
      // If the predecessor has already been scheduled, approximate its
      // logical distance to SU as (CurIndex - order[Pred]).
      auto ItOrd = ScheduledOrder.find(Pred);
      if (ItOrd != ScheduledOrder.end() && CurIndex > ItOrd->second) {
        unsigned Dist = CurIndex - ItOrd->second;
        // Use predecessor static weight as an importance proxy.
        float W = (It != StaticWeight.end()) ? It->second : 1.0f;
        DistanceCost += W * static_cast<float>(Dist);
      }
    }
  }
  // Apply a small penalty proportional to the accumulated w·ℓ distance to
  // favor schedules that keep heavily-related instructions close.
  if (DistanceCost > 0.0f) {
    constexpr float DistanceScale = 0.01f;
    priority -= DistanceScale * DistanceCost;
  }

  // Simple dynamic penalty: discourage scheduling loads/stores early when
  // they may cause cache misses (MissPenaltyScale tuned via cl::opt).
  if (SU->getInstr() && SU->getInstr()->mayLoadOrStore()) {
    priority -= MissPenaltyScale;
  }
  // If improved ctrl metric enabled, penalize by averaged CFG distance to
  // control successors (normalized). This requires computing MBB distances.
  if (UseImprovedCtrl) {
    // src MBB
    const MachineInstr *SrcMI = SU->getInstr();
    const MachineBasicBlock *SrcMBB = SrcMI ? SrcMI->getParent() : nullptr;
    unsigned sumDist = 0;
    unsigned cnt = 0;
    for (const SDep &SuccDep : SU->Succs) {
      if (SuccDep.isCtrl()) {
        const SUnit *T = SuccDep.getSUnit();
        if (!T) continue;
        const MachineInstr *DstMI = T->getInstr();
        const MachineBasicBlock *DstMBB = DstMI ? DstMI->getParent() : nullptr;
        unsigned d = 64;
        if (SrcMBB && DstMBB) {
          // check cache
          auto ItOuter = MBBDistCache.find(SrcMBB);
          if (ItOuter != MBBDistCache.end()) {
            auto &inner = ItOuter->second;
            auto ItInner = inner.find(DstMBB);
            if (ItInner != inner.end())
              d = ItInner->second;
          }
          if (d == 64) {
            // BFS from SrcMBB to DstMBB (cap 64)
            SmallVector<const MachineBasicBlock *, 64> WorkList;
            DenseMap<const MachineBasicBlock *, unsigned> Dist;
            WorkList.push_back(SrcMBB);
            Dist[SrcMBB] = 0;
            for (size_t i = 0; i < WorkList.size(); ++i) {
              const MachineBasicBlock *MBB = WorkList[i];
              unsigned dist = Dist[MBB];
              if (dist >= 64) break;
              for (const auto *Succ : MBB->successors()) {
                if (!Dist.count(Succ)) {
                  Dist[Succ] = dist + 1;
                  WorkList.push_back(Succ);
                  if (Succ == DstMBB) {
                    d = Dist[Succ];
                    break;
                  }
                }
              }
              if (d != 64) break;
            }
            MBBDistCache[SrcMBB][DstMBB] = d;
          }
        }
        sumDist += d;
        ++cnt;
      }
    }
    if (cnt > 0) {
      float avg = static_cast<float>(sumDist) / static_cast<float>(cnt);
      // normalize to (0..1) with soft saturation
      float ctrlNorm = avg / (avg + 1.0f);
      // apply as additional penalty (increase ctrl effect)
      priority -= Gamma * ctrlNorm;
    }
  }

  return priority;
}

void SIDSScheduler::schedNode(SUnit *SU, bool IsTopNode) {
  // Called when a node is scheduled. Use to:
  //  - record its position in the linearized schedule (for distance estimates)
  //  - collect simple statistics for the adaptive miss-penalty simulation.
  if (SU)
    ScheduledOrder[SU] = ScheduledCount++;

  if (!AdaptiveMissPenalty)
    return;

  if (SU && SU->getInstr() && SU->getInstr()->mayLoadOrStore()) {
    ++AdaptiveLoadCount;
  }

  // When window is filled, compute a simple estimate and update MissPenaltyScale
  static unsigned WindowProgress = 0;
  ++WindowProgress;
  if (WindowProgress >= AdaptiveWindow) {
    // measured := fraction of loads scheduled in window
    float measured = static_cast<float>(AdaptiveLoadCount) / static_cast<float>(AdaptiveWindow);
    // Map measured [0..1] to an estimated penalty in [0..1] and update via EMA
    float estimatedPenalty = std::min(1.0f, measured * 1.0f);
    float newPenalty = (1.0f - AdaptiveBeta) * MissPenaltyScale + AdaptiveBeta * estimatedPenalty;
    LLVM_DEBUG(dbgs() << "SIDS adaptive: window=" << AdaptiveWindow << " loads=" << AdaptiveLoadCount
                      << " measured=" << measured << " oldPenalty=" << MissPenaltyScale
                      << " newPenalty=" << newPenalty << "\n");
    MissPenaltyScale = newPenalty;
    // reset counters
    AdaptiveLoadCount = 0;
    WindowProgress = 0;
  }
}

SUnit *SIDSScheduler::pickNode(bool &IsTopNode) {
  SUnit *Best = nullptr;
  float BestPriority = -std::numeric_limits<float>::infinity();
  IsTopNode = true;

  for (SUnit &SU : DAG->SUnits) {
    if (SU.isScheduled || (!SU.isTopReady() && !SU.isBottomReady()))
      continue;

    float Prio = computeSIDSPriority(&SU);
    if (Prio > BestPriority) {
      BestPriority = Prio;
      Best = &SU;
      IsTopNode = SU.isTopReady();
    }
  }

  if (Best) {
    LLVM_DEBUG({
      dbgs() << "SIDS picked: SU#" << Best->NodeNum
             << " Priority=" << BestPriority
             << " StaticW=" << (StaticWeight.count(Best) ? StaticWeight.lookup(Best) : -0.0f)
             << " isInst=" << (Best->getInstr() != nullptr);
      if (Best->getInstr()) {
        dbgs() << " opcode=" << Best->getInstr()->getOpcode();
        dbgs() << " instr=";
        Best->getInstr()->print(dbgs(), /*IsStandalone=*/true);
      } else {
        dbgs() << " instr=<Entry/Exit>";
      }
      dbgs() << "\n";
    });
  }

  return Best;
}