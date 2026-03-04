// llvm/lib/CodeGen/SIDSScheduler.cpp
#include "llvm/CodeGen/SIDSScheduler.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "sids-scheduler"

using namespace llvm;

void SIDSScheduler::initialize(ScheduleDAGMI *dag) {
  DAG = dag;

  // 示例初始化：这里可以从 MachineFunction 的 Metadata 或外部文件加载权重
  // 目前使用简单模拟：优先级基于节点深度 + 指令类型
  for (SUnit &SU : DAG->SUnits) {
    // 模拟权重计算（实际应替换为您的静态分析结果）
    float reuse = 1.0f;   // 数据复用度（DU链分析）
    float hot   = 1.0f;   // 执行热点（PGO或循环深度）
    float ctrl  = 0.0f;   // 控制距离（后支配树距离）

    // 简单模拟：指令越靠后，控制距离惩罚越大
    ctrl = static_cast<float>(SU.NodeNum) / DAG->SUnits.size();

    float weight = Alpha * reuse + Beta * hot - Gamma * ctrl;
    StaticWeight[&SU] = weight;
  }

  LLVM_DEBUG(dbgs() << "SIDS Scheduler initialized. Total SUnits: "
                    << DAG->SUnits.size() << "\n");
}

float SIDSScheduler::computeSIDSPriority(const SUnit *SU) const {
  float priority = 0.0f;

  // 继承前驱最大权重（体现数据“渴望”程度）
  for (const SDep &PredDep : SU->Preds) {
    if (PredDep.getKind() == SDep::Data) {
      const SUnit *Pred = PredDep.getSUnit();
      auto It = StaticWeight.find(Pred);
      if (It != StaticWeight.end()) {
        priority = std::max(priority, It->second);
      }
    }
  }

  // 后续可扩展：
  // - 动态距离惩罚：与上一条已调度指令的控制流距离
  // - MissPenalty：基于 perf 反馈的缓存缺失惩罚

  return priority;
}

SUnit *SIDSScheduler::pickNode(bool &IsTopNode) {
  SUnit *Best = nullptr;
  float BestPriority = -1.0f;
  IsTopNode = true;  // 默认使用 Top 调度（可根据需要切换 Bottom）

  // 遍历就绪队列，挑选优先级最高的节点
  for (SUnit *SU : DAG->TopReadyQ) {
    float Prio = computeSIDSPriority(SU);
    if (Prio > BestPriority) {
      BestPriority = Prio;
      Best = SU;
    }
  }

  if (Best) {
    LLVM_DEBUG(dbgs() << "SIDS picked: "; Best->dumpAll(););
  }

  return Best;
}