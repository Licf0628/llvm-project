# SIDS 集成手册 - 第二部分：核心实现详解

## 文件信息

- **文件路径**：`llvm/lib/CodeGen/SIDSScheduler.cpp`
- **作用**：实现 SIDS 调度器的核心算法
- **代码行数**：约 363 行

---

## 文件结构概览

```
1. 头文件包含 (行 1-10)
2. 命令行参数定义 (行 97-115)
3. 工厂函数与注册 (行 117-118)
4. initialize() 实现 (行 120-170)
5. computeSIDSPriority() 实现 (行 175-257)
6. schedNode() 实现 (行 259-325)
7. pickNode() 实现 (行 328-363)
```

---

## 1. 头文件包含

```cpp
#include "llvm/CodeGen/SIDSScheduler.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/CodeGen/MachineScheduler.h"

#define DEBUG_TYPE "sids-scheduler"
```

### 为什么需要这些头文件？

| 头文件 | 提供的功能 | 使用场景 |
|--------|-----------|---------|
| `SIDSScheduler.h` | 类定义 | 必须包含自己的头文件 |
| `MachineInstr.h` | `MachineInstr` 类 | 访问指令信息（`mayLoadOrStore()`） |
| `Debug.h` | `LLVM_DEBUG` 宏 | 调试输出 |
| `raw_ostream.h` | `dbgs()` 函数 | 输出到调试流 |
| `MachineBasicBlock.h` | `MachineBasicBlock` 类 | CFG 距离计算 |
| `MachineFunction.h` | `MachineFunction` 类 | 访问函数级信息 |
| `CommandLine.h` | `cl::opt` 模板 | 命令行参数 |
| `MachineScheduler.h` | `ScheduleDAGMI` 等 | 调度框架 |

### DEBUG_TYPE 宏

```cpp
#define DEBUG_TYPE "sids-scheduler"
```

**作用**：定义调试输出的类型标识

**使用方式**：
```bash
llc -debug-only=sids-scheduler test.ll
```

**原理**：
- `LLVM_DEBUG` 宏会检查 `DEBUG_TYPE`
- 只有匹配的类型才会输出

---

## 2. 命令行参数定义

```cpp
static cl::opt<float> SIDSAlpha("sids-alpha", 
                                cl::desc("SIDS Reuse weight"), 
                                cl::init(0.4));
static cl::opt<float> SIDSBeta("sids-beta",  
                               cl::desc("SIDS Hot weight"),   
                               cl::init(0.4));
static cl::opt<float> SIDSGamma("sids-gamma", 
                                cl::desc("SIDS Ctrl penalty"), 
                                cl::init(0.2));
static cl::opt<float> SIDSMissPenalty("sids-miss-penalty",
                                      cl::desc("SIDS Miss penalty scale"),
                                      cl::init(0.5f));
static cl::opt<bool> SIDSMissPenaltyAdaptive("sids-miss-penalty-adaptive",
                                              cl::desc("Enable adaptive miss penalty"),
                                              cl::init(false));
static cl::opt<float> SIDSMissPenaltyBeta("sids-miss-penalty-beta",
                                           cl::desc("Adaptive smoothing factor"),
                                           cl::init(0.2f));
static cl::opt<unsigned> SIDSMissPenaltyWindow("sids-miss-penalty-window",
                                                cl::desc("Adaptive update window"),
                                                cl::init(32));
static cl::opt<bool> SIDSUseImprovedCtrl("sids-ctrl-improved",
                                          cl::desc("Use CFG-based control distance"),
                                          cl::init(false));
```

### cl::opt 模板详解

**语法**：
```cpp
cl::opt<类型> 变量名("命令行名", cl::desc("描述"), cl::init(默认值));
```

**为什么用 static**：
- 文件作用域，不暴露给外部
- 避免符号冲突

**为什么用 cl::opt 而非普通变量**：
- 自动注册到 LLVM 命令行系统
- 自动生成 `--help` 文档
- 类型安全的解析

**使用示例**：
```bash
llc -sids-alpha=0.7 -sids-beta=0.2 -sids-gamma=0.1 test.ll
```

---

## 3. 工厂函数与注册

```cpp
std::unique_ptr<MachineSchedStrategy> createSIDSScheduler() {
  return std::make_unique<SIDSScheduler>();
}

static MachineSchedRegistry
SIDSRegistry("sids", "SIDS scheduler", createSIDSScheduler);
```

### 工厂函数

**作用**：创建 SIDS 调度器实例

**返回类型**：`std::unique_ptr<MachineSchedStrategy>`
- 智能指针，自动管理生命周期
- 返回基类指针，支持多态

### 注册机制

```cpp
static MachineSchedRegistry SIDSRegistry(
    "sids",                  // 调度器名称
    "SIDS scheduler",        // 描述
    createSIDSScheduler      // 工厂函数
);
```

**原理**：
- `MachineSchedRegistry` 的构造函数会将调度器注册到全局表
- 使用 `-misched=sids` 时，LLVM 会查找并调用 `createSIDSScheduler()`

**为什么用 static**：
- 在程序启动时自动注册
- 利用 C++ 静态对象初始化机制

---

## 4. initialize() 实现

### 4.1 函数签名

```cpp
void SIDSScheduler::initialize(ScheduleDAGMI *dag) {
  DAG = dag;
  StaticWeight.clear();
  ScheduledOrder.clear();
  ScheduledCount = 0;
  MBBDistCache.clear();
```

**参数**：`ScheduleDAGMI *dag`
- 指向当前调度区域的 DAG
- 包含所有 SUnit 和依赖信息

**初始化步骤**：
1. 保存 DAG 指针
2. 清空缓存（支持多次调用）
3. 重置计数器

### 4.2 读取命令行参数

```cpp
  Alpha = SIDSAlpha;
  Beta  = SIDSBeta;
  Gamma = SIDSGamma;
  MissPenaltyScale = SIDSMissPenalty;
  AdaptiveMissPenalty = SIDSMissPenaltyAdaptive;
  AdaptiveBeta = SIDSMissPenaltyBeta;
  AdaptiveWindow = SIDSMissPenaltyWindow;
  AdaptiveLoadCount = 0;
```

**为什么在这里读取**：
- `initialize()` 在每个调度区域开始时调用
- 确保使用最新的命令行参数
- 支持运行时修改（虽然不常用）

### 4.3 计算静态权重

#### 核心循环

```cpp
for (SUnit &SU : DAG->SUnits) {
    // 计算 Reuse, Hot, Ctrl
    // ...
    float weight = Alpha * reuse + Beta * hot - Gamma * ctrl;
    StaticWeight[&SU] = weight;
}
```

#### 4.3.1 计算 Reuse（数据复用度）

```cpp
// ---- Reuse: 近似"定义->使用"平均距离 ----
unsigned SumDist = 0;
unsigned DistCount = 0;
for (const SDep &SuccDep : SU.Succs) {
  if (SuccDep.getKind() == SDep::Data) {
    if (const SUnit *Succ = SuccDep.getSUnit()) {
      unsigned DefDepth = SU.getDepth();
      unsigned UseDepth = Succ->getDepth();
      if (UseDepth >= DefDepth) {
        SumDist += (UseDepth - DefDepth);
        ++DistCount;
      }
    }
  }
}
float AvgDist = DistCount ? static_cast<float>(SumDist) / DistCount : 0.0f;
float reuse = 1.0f / (AvgDist + 1.0f);
```

**算法解析**：

1. **遍历数据后继**
   - `SU.Succs`：所有后继依赖
   - `SuccDep.getKind() == SDep::Data`：只考虑数据依赖

2. **计算深度差**
   - `SU.getDepth()`：从根节点到 SU 的最长路径
   - `Succ->getDepth()`：从根节点到后继的最长路径
   - 差值近似"定义到使用"的逻辑距离

3. **转换为复用度**
   - `1 / (avgDist + 1)`：距离越短，复用度越高
   - `+1` 避免除零

**为什么用 Depth 差值**：
- DAG 中没有直接的"指令距离"信息
- Depth 是 LLVM 已经计算好的属性
- 近似效果足够好

#### 4.3.2 计算 Hot（热点）

```cpp
// ---- Hot: 使用节点深度作为关键路径的粗略代理 ----
float hot = 1.0f + static_cast<float>(SU.getDepth()) * 0.1f;
```

**算法解析**：

1. **基础值 1.0**
   - 确保所有节点都有基础热点值

2. **深度加成**
   - `Depth * 0.1`：深度越大，热点值越高
   - 系数 0.1 是经验值，避免热点权重过大

**为什么用 Depth**：
- 关键路径上的节点 Depth 较大
- 简单有效的热点近似

**改进方向**：
- 结合 PGO 数据（块频度）
- 检测循环内的节点

#### 4.3.3 计算 Ctrl（控制距离）

```cpp
// ---- Ctrl: 使用 NodeNum 归一化作为控制距离的近似 ----
float ctrl = static_cast<float>(SU.NodeNum) / 
             std::max<size_t>(1, DAG->SUnits.size());
```

**算法解析**：

1. **NodeNum 归一化**
   - `NodeNum`：SUnit 在 DAG 中的编号（0 到 N-1）
   - 除以总数得到 0.0 到 1.0 的值

2. **为什么用 NodeNum**：
   - 简单快速
   - 后面的节点通常控制距离更大

**改进版本**（`-sids-ctrl-improved`）：
- 在 `computeSIDSPriority()` 中用 CFG BFS 计算真实距离
- 见下文详解

#### 4.3.4 综合权重

```cpp
float weight = Alpha * reuse + Beta * hot - Gamma * ctrl;
StaticWeight[&SU] = weight;
```

**公式**：
```
W(u) = α·Reuse(u) + β·Hot(u) - γ·Ctrl(u)
```

**为什么 Ctrl 是减法**：
- 控制距离大是坏事，应该降低权重
- Reuse 和 Hot 是好事，应该提高权重

### 4.4 调试输出

```cpp
LLVM_DEBUG(dbgs() << "=== SIDS Scheduler initialized ===\n"
                  << "  Alpha=" << Alpha << " Beta=" << Beta 
                  << " Gamma=" << Gamma << " MissPenalty=" << MissPenaltyScale << "\n"
                  << "  Total SUnits: " << DAG->SUnits.size() << "\n");
```

**LLVM_DEBUG 宏**：
- 只在 `LLVM_ENABLE_ASSERTIONS=ON` 时编译
- 通过 `-debug-only=sids-scheduler` 启用

---

## 5. computeSIDSPriority() 实现

### 5.1 函数签名

```cpp
float SIDSScheduler::computeSIDSPriority(const SUnit *SU) const {
```

**const 函数**：
- 不修改 `SIDSScheduler` 的状态
- 只读取 `StaticWeight`、`ScheduledOrder` 等

### 5.2 初始化

```cpp
float priority = 0.0f;
float DistanceCost = 0.0f;
unsigned CurIndex = ScheduledCount;
```

**CurIndex**：
- 假设下一步调度 SU
- 用于计算距离

### 5.3 继承前驱权重 + 计算距离代价

```cpp
for (const SDep &PredDep : SU->Preds) {
  if (PredDep.getKind() == SDep::Data) {
    const SUnit *Pred = PredDep.getSUnit();
    auto It = StaticWeight.find(Pred);
    if (It != StaticWeight.end()) {
      priority = std::max(priority, It->second);
    }
    
    // 计算 w·ℓ 距离代价
    auto ItOrd = ScheduledOrder.find(Pred);
    if (ItOrd != ScheduledOrder.end() && CurIndex > ItOrd->second) {
      unsigned Dist = CurIndex - ItOrd->second;
      float W = (It != StaticWeight.end()) ? It->second : 1.0f;
      DistanceCost += W * static_cast<float>(Dist);
    }
  }
}
```

**算法解析**：

1. **继承前驱权重**
   ```cpp
   priority = std::max(priority, It->second);
   ```
   - 取所有数据前驱的最大权重
   - 确保依赖链上的节点优先级递减

2. **计算距离代价**
   ```cpp
   unsigned Dist = CurIndex - ItOrd->second;
   DistanceCost += W * Dist;
   ```
   - 只对已调度的前驱计算
   - `Dist`：当前位置到前驱的距离
   - `W`：前驱的静态权重
   - 累加 `w·ℓ`

**为什么只考虑已调度的前驱**：
- 未调度的前驱距离未知
- 已调度的前驱距离确定

### 5.4 应用距离惩罚

```cpp
if (DistanceCost > 0.0f) {
  constexpr float DistanceScale = 0.01f;
  priority -= DistanceScale * DistanceCost;
}
```

**DistanceScale = 0.01**：
- 距离代价的缩放系数
- 避免距离惩罚过大
- 经验值，可调整

**效果**：
- 距离代价大 → 优先级降低
- 让高权重依赖的指令尽量靠近

### 5.5 Cache Miss 惩罚

```cpp
if (SU->getInstr() && SU->getInstr()->mayLoadOrStore()) {
  priority -= MissPenaltyScale;
}
```

**mayLoadOrStore()**：
- LLVM 提供的方法
- 检查指令是否访问内存

**效果**：
- Load/Store 指令优先级降低
- 被推迟调度

### 5.6 改进版控制距离（可选）

```cpp
if (UseImprovedCtrl) {
  const MachineInstr *SrcMI = SU->getInstr();
  const MachineBasicBlock *SrcMBB = SrcMI ? SrcMI->getParent() : nullptr;
  
  unsigned sumDist = 0;
  unsigned cnt = 0;
  
  for (const SDep &SuccDep : SU->Succs) {
    if (SuccDep.isCtrl()) {
      const SUnit *T = SuccDep.getSUnit();
      // ... 计算 CFG 距离 ...
      unsigned d = computeMBBDistance(SrcMBB, DstMBB);
      sumDist += d;
      cnt++;
    }
  }
  
  if (cnt > 0) {
    float avgDist = static_cast<float>(sumDist) / cnt;
    float ctrlNorm = avgDist / 64.0f;  // 归一化
    priority -= Gamma * ctrlNorm;
  }
}
```

**CFG BFS 算法**（简化版）：

```cpp
SmallVector<const MachineBasicBlock *, 64> WorkList;
DenseMap<const MachineBasicBlock *, unsigned> Dist;

WorkList.push_back(SrcMBB);
Dist[SrcMBB] = 0;

while (!WorkList.empty()) {
  const MachineBasicBlock *Cur = WorkList.pop_back_val();
  unsigned curDist = Dist[Cur];
  
  if (Cur == DstMBB) {
    d = curDist;
    break;
  }
  
  if (curDist >= 64) continue;  // 限制搜索深度
  
  for (const MachineBasicBlock *Succ : Cur->successors()) {
    if (Dist.find(Succ) == Dist.end()) {
      Dist[Succ] = curDist + 1;
      WorkList.push_back(Succ);
    }
  }
}
```

**为什么需要缓存**：
- BFS 开销大（O(V+E)）
- 同一对 MBB 可能被多次查询
- 缓存后 O(1) 查找

---

## 6. schedNode() 实现

### 6.1 记录调度顺序

```cpp
void SIDSScheduler::schedNode(SUnit *SU, bool IsTopNode) {
  if (SU)
    ScheduledOrder[SU] = ScheduledCount++;
```

**作用**：
- 记录 SU 的调度位置
- 用于后续的距离计算

### 6.2 自适应 Miss 惩罚（可选）

```cpp
if (!AdaptiveMissPenalty)
  return;

if (SU && SU->getInstr() && SU->getInstr()->mayLoadOrStore()) {
  ++AdaptiveLoadCount;
}

static unsigned WindowProgress = 0;
++WindowProgress;

if (WindowProgress >= AdaptiveWindow) {
  float measured = static_cast<float>(AdaptiveLoadCount) / AdaptiveWindow;
  float estimatedPenalty = std::min(1.0f, measured * 1.0f);
  float newPenalty = (1.0f - AdaptiveBeta) * MissPenaltyScale 
                   + AdaptiveBeta * estimatedPenalty;
  
  LLVM_DEBUG(dbgs() << "SIDS adaptive: window=" << AdaptiveWindow 
                    << " loads=" << AdaptiveLoadCount
                    << " measured=" << measured 
                    << " oldPenalty=" << MissPenaltyScale
                    << " newPenalty=" << newPenalty << "\n");
  
  MissPenaltyScale = newPenalty;
  AdaptiveLoadCount = 0;
  WindowProgress = 0;
}
```

**算法**：指数移动平均（EMA）

**公式**：
```
newPenalty = (1-β) * oldPenalty + β * measured
```

**原理**：
- 统计窗口内 load/store 的比例
- 比例高 → 提高 miss 惩罚
- 比例低 → 降低 miss 惩罚

**为什么用 static**：
- `WindowProgress` 需要跨调用保持
- 每个调度区域独立计数

---

## 7. pickNode() 实现

### 7.1 初始化

```cpp
SUnit *SIDSScheduler::pickNode(bool &IsTopNode) {
  SUnit *Best = nullptr;
  float BestPriority = -std::numeric_limits<float>::infinity();
  IsTopNode = true;
```

**关键修复**：
- 使用 `-∞` 而非 `-1.0f`
- 确保负优先级节点也能被选中

### 7.2 遍历候选节点

```cpp
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
```

**过滤条件**：
1. `SU.isScheduled`：跳过已调度节点
2. `!SU.isTopReady() && !SU.isBottomReady()`：跳过未就绪节点

**为什么遍历所有 SUnit**：
- 简化实现，不维护就绪队列
- 性能影响小（SUnit 数量通常不多）

### 7.3 调试输出

```cpp
if (Best) {
  LLVM_DEBUG({
    dbgs() << "SIDS picked: SU#" << Best->NodeNum
           << " Priority=" << BestPriority
           << " StaticW=" << (StaticWeight.count(Best) ? 
                              StaticWeight.lookup(Best) : -0.0f)
           << " isInst=" << (Best->getInstr() != nullptr)
           << " opcode=" << (Best->getInstr() ? 
                             Best->getInstr()->getOpcode() : 0)
           << " instr=";
    if (Best->getInstr())
      Best->getInstr()->print(dbgs(), /*IsStandalone=*/true);
    else
      dbgs() << "(Entry/Exit)";
    dbgs() << "\n";
  });
}
```

**输出格式**：
```
SIDS picked: SU#3 Priority=0.750 StaticW=0.850 isInst=1 opcode=365 instr=%10:i32 = ADD_I32 ...
```

---

## 关键设计决策总结

### 1. 为什么用 DenseMap 而非 vector？

**优点**：
- 不需要 SUnit 有连续 ID
- 插入/查找都是 O(1)
- 内存紧凑

### 2. 为什么在 pickNode 中遍历所有 SUnit？

**原因**：
- 简化实现
- SUnit 数量通常不多（几十到几百）
- 性能影响可接受

**替代方案**：
- 维护就绪队列（更复杂）

### 3. 为什么用 -∞ 作为初始优先级？

**原因**：
- 支持负优先级节点
- 避免 `-1.0f` 导致的 bug

### 4. 为什么距离代价用 0.01 缩放？

**原因**：
- 避免距离惩罚过大
- 保持与静态权重同一数量级

---

## 下一步

继续阅读：
- [第三部分：后端集成](./SIDS_MANUAL_PART3_BACKEND.md)
- [第四部分：构建配置](./SIDS_MANUAL_PART4_BUILD.md)
