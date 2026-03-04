# SIDS 调度器 LLVM 集成完整手册

## 目录

1. [概述](#1-概述)
2. [核心文件清单](#2-核心文件清单)
3. [详细实现指南](#3-详细实现指南)
4. [数据结构设计](#4-数据结构设计)
5. [算法实现](#5-算法实现)
6. [集成与测试](#6-集成与测试)
7. [参数调优](#7-参数调优)

---

## 1. 概述

### 1.1 SIDS 调度器简介

SIDS (Shortest Instruction Distance Scheduler) 是一个基于"最短指令距离"思想的编译期指令调度器，目标是最小化加权指令距离：

```
min Σ w(u,v) · ℓ(u,v)
```

其中：
- `w(u,v)` = 边权重，由数据复用度、热点、控制距离综合决定
- `ℓ(u,v)` = 指令 u 到 v 在最终调度序列中的距离

### 1.2 集成架构

```
LLVM IR
   ↓
SelectionDAG / FastISel
   ↓
MachineInstr (未调度)
   ↓
MachineScheduler ← SIDS 调度器插入点
   ↓
Register Allocation
   ↓
Assembly / Object Code
```

### 1.3 设计目标

1. **最小化指令距离**：让高权重依赖的指令尽量靠近
2. **提高缓存局部性**：推迟 load/store 指令，减少 cache miss
3. **优化热点路径**：优先调度关键路径上的指令
4. **可配置性**：通过命令行参数适应不同场景

---

## 2. 核心文件清单

### 2.1 新增文件

| 文件路径 | 作用 | 代码行数 |
|---------|------|---------|
| `llvm/include/llvm/CodeGen/SIDSScheduler.h` | SIDS 调度器头文件 | ~80 行 |
| `llvm/lib/CodeGen/SIDSScheduler.cpp` | SIDS 调度器实现 | ~363 行 |

### 2.2 修改文件

| 文件路径 | 修改内容 | 原因 |
|---------|---------|------|
| `llvm/lib/CodeGen/CMakeLists.txt` | 添加 `SIDSScheduler.cpp` | 将新文件加入构建系统 |
| `llvm/lib/Target/WebAssembly/WebAssemblyTargetMachine.h` | 声明 `createMachineScheduler` | 为 wasm 后端启用自定义调度器 |
| `llvm/lib/Target/WebAssembly/WebAssemblyTargetMachine.cpp` | 实现 `createMachineScheduler` | 默认使用 SIDS 调度器 |

### 2.3 文件依赖关系

```
SIDSScheduler.h
    ↓ 包含
SIDSScheduler.cpp
    ↓ 依赖
llvm/CodeGen/MachineScheduler.h
llvm/CodeGen/ScheduleDAG.h
llvm/CodeGen/TargetSchedule.h
    ↓ 使用
WebAssemblyTargetMachine.cpp
```

---

## 3. 详细实现指南

详细内容请参考以下分册：

- [第一部分：头文件设计](./SIDS_MANUAL_PART1_HEADER.md)
- [第二部分：核心实现](./SIDS_MANUAL_PART2_IMPL.md)
- [第三部分：后端集成](./SIDS_MANUAL_PART3_BACKEND.md)
- [第四部分：构建配置](./SIDS_MANUAL_PART4_BUILD.md)

---

## 4. 数据结构设计

### 4.1 核心数据结构

#### 4.1.1 SIDSScheduler 类

```cpp
class SIDSScheduler : public MachineSchedStrategy {
private:
  // 调度上下文
  ScheduleDAGMI *DAG = nullptr;
  
  // 静态权重缓存
  DenseMap<const SUnit *, float> StaticWeight;
  
  // 调度顺序跟踪（用于距离计算）
  DenseMap<const SUnit *, unsigned> ScheduledOrder;
  unsigned ScheduledCount = 0;
  
  // 权重参数
  float Alpha = 0.4f;  // 数据复用度权重
  float Beta  = 0.4f;  // 热点权重
  float Gamma = 0.2f;  // 控制距离惩罚
  
  // Cache miss 惩罚
  float MissPenaltyScale = 0.5f;
  
  // 自适应 miss 惩罚（可选）
  bool AdaptiveMissPenalty = false;
  float AdaptiveBeta = 0.2f;
  unsigned AdaptiveWindow = 32;
  unsigned AdaptiveLoadCount = 0;
  
  // MBB 距离缓存（用于改进版控制距离）
  DenseMap<const MachineBasicBlock*, 
           DenseMap<const MachineBasicBlock*, unsigned>> MBBDistCache;
};
```

**设计理由**：

1. **DenseMap 用于权重缓存**
   - 原因：SUnit 数量通常不多（几十到几百），DenseMap 提供 O(1) 查找
   - 替代方案：vector（需要 SUnit 有连续 ID）

2. **ScheduledOrder 跟踪调度顺序**
   - 原因：实现 w·ℓ 距离计算需要知道每个节点的调度位置
   - 用途：在 `computeSIDSPriority` 中计算距离惩罚

3. **MBBDistCache 两层 Map**
   - 原因：CFG 上的 BFS 距离计算开销大，需要缓存
   - 结构：`MBB_src -> (MBB_dst -> distance)`

### 4.2 LLVM 提供的数据结构

#### 4.2.1 SUnit (Scheduling Unit)

```cpp
struct SUnit {
  MachineInstr *Instr;           // 对应的机器指令
  SmallVector<SDep, 4> Preds;    // 前驱依赖
  SmallVector<SDep, 4> Succs;    // 后继依赖
  unsigned NodeNum;              // 节点编号
  unsigned Depth;                // 从根节点的最长路径
  unsigned Height;               // 到叶节点的最长路径
  bool isScheduled;              // 是否已调度
};
```

**我们如何使用**：
- `Succs` 中的 `SDep::Data` 用于计算数据复用度
- `Depth` 用于近似热点路径
- `NodeNum` 用于近似控制距离
- `isScheduled` 用于过滤候选节点

#### 4.2.2 SDep (Scheduling Dependency)

```cpp
struct SDep {
  enum Kind {
    Data,     // 数据依赖（RAW, WAR, WAW）
    Anti,     // 反依赖
    Output,   // 输出依赖
    Order     // 顺序依赖
  };
  
  SUnit *Dep;           // 依赖的目标节点
  Kind DepKind;         // 依赖类型
  unsigned Latency;     // 延迟周期数
};
```

**我们如何使用**：
- `DepKind == Data` 用于识别数据依赖边
- `isCtrl()` 用于识别控制依赖
- `Dep->getDepth()` 用于计算定义-使用距离

---

## 5. 算法实现

### 5.1 初始化阶段（initialize）

**目的**：为每个 SUnit 计算静态权重

**算法流程**：

```
for each SUnit in DAG:
    1. 计算 Reuse（数据复用度）
       - 遍历所有数据后继
       - 计算平均"定义-使用"距离
       - Reuse = 1 / (avgDist + 1)
    
    2. 计算 Hot（热点）
       - 使用节点深度作为代理
       - Hot = 1 + Depth * 0.1
    
    3. 计算 Ctrl（控制距离）
       - 使用 NodeNum 归一化
       - Ctrl = NodeNum / |V|
    
    4. 综合权重
       - StaticWeight[SU] = Alpha*Reuse + Beta*Hot - Gamma*Ctrl
```

**代码位置**：`SIDSScheduler.cpp:120-170`

### 5.2 节点选择阶段（pickNode）

**目的**：从就绪队列中选择优先级最高的节点

**算法流程**：

```
BestPriority = -∞
Best = nullptr

for each SUnit in DAG:
    if SU.isScheduled or not SU.isReady():
        continue
    
    Priority = computeSIDSPriority(SU)
    
    if Priority > BestPriority:
        BestPriority = Priority
        Best = SU

return Best
```

**关键点**：
- 使用 `-∞` 作为初始值，确保负优先级节点也能被选中
- 遍历所有 SUnit 而非维护就绪队列（简化实现）

**代码位置**：`SIDSScheduler.cpp:328-345`

### 5.3 优先级计算（computeSIDSPriority）

**目的**：计算节点的动态优先级

**算法流程**：

```
Priority = 0
DistanceCost = 0
CurIndex = ScheduledCount  // 假设下一步调度 SU

// 1. 继承前驱最大权重
for each Pred in SU.Preds where Pred.kind == Data:
    Priority = max(Priority, StaticWeight[Pred])
    
    // 2. 计算 w·ℓ 距离惩罚
    if Pred is scheduled:
        Dist = CurIndex - ScheduledOrder[Pred]
        W = StaticWeight[Pred]
        DistanceCost += W * Dist

// 3. 应用距离惩罚
Priority -= DistanceScale * DistanceCost

// 4. Cache miss 惩罚
if SU.mayLoadOrStore():
    Priority -= MissPenaltyScale

// 5. 改进版控制距离惩罚（可选）
if UseImprovedCtrl:
    for each CtrlSucc in SU.Succs where CtrlSucc.isCtrl():
        CFG_Dist = computeMBBDistance(SU.MBB, CtrlSucc.MBB)
        Priority -= Gamma * normalize(CFG_Dist)

return Priority
```

**关键设计**：

1. **继承前驱权重**
   - 原因：数据依赖链上的节点应该连续调度
   - 实现：取所有数据前驱的最大静态权重

2. **w·ℓ 距离惩罚**
   - 原因：实现"最短指令距离"的核心思想
   - 实现：对已调度的前驱，计算距离并按权重惩罚

3. **Cache miss 惩罚**
   - 原因：内存访问延迟高，应推迟调度
   - 实现：对 load/store 指令统一扣分

**代码位置**：`SIDSScheduler.cpp:175-257`

### 5.4 调度记录（schedNode）

**目的**：记录节点的调度位置，用于后续距离计算

**算法流程**：

```
if SU != nullptr:
    ScheduledOrder[SU] = ScheduledCount
    ScheduledCount++

// 自适应 miss 惩罚（可选）
if AdaptiveMissPenalty and SU.mayLoadOrStore():
    AdaptiveLoadCount++
    
    if ScheduledCount % AdaptiveWindow == 0:
        measured = AdaptiveLoadCount / AdaptiveWindow
        estimatedPenalty = min(1.0, measured)
        MissPenaltyScale = (1-AdaptiveBeta)*MissPenaltyScale 
                         + AdaptiveBeta*estimatedPenalty
        AdaptiveLoadCount = 0
```

**代码位置**：`SIDSScheduler.cpp:259-325`

---

## 6. 集成与测试

### 6.1 构建系统集成

#### 6.1.1 修改 CMakeLists.txt

**文件**：`llvm/lib/CodeGen/CMakeLists.txt`

**修改内容**：
```cmake
add_llvm_component_library(LLVMCodeGen
  # ... 其他文件 ...
  SIDSScheduler.cpp        # ← 添加这一行
  # ... 其他文件 ...
)
```

**位置**：在文件中找到 `add_llvm_component_library(LLVMCodeGen` 部分，在源文件列表中添加 `SIDSScheduler.cpp`

**原因**：
- LLVM 使用 CMake 管理构建
- 必须将新文件加入 `LLVMCodeGen` 组件才能被编译和链接

### 6.2 WebAssembly 后端集成

#### 6.2.1 修改头文件

**文件**：`llvm/lib/Target/WebAssembly/WebAssemblyTargetMachine.h`

**修改内容**：
```cpp
class WebAssemblyTargetMachine final : public LLVMTargetMachine {
  // ... 现有成员 ...
  
public:
  // ... 现有方法 ...
  
  // 启用 MachineScheduler
  bool enableMachineScheduler() const override { return true; }
  
  // 创建自定义调度器
  ScheduleDAGInstrs *
  createMachineScheduler(MachineSchedContext *C) const override;
};
```

**原因**：
- `enableMachineScheduler()` 告诉 LLVM 为 wasm 目标启用调度 pass
- `createMachineScheduler()` 允许我们返回自定义的调度器实例

#### 6.2.2 修改实现文件

**文件**：`llvm/lib/Target/WebAssembly/WebAssemblyTargetMachine.cpp`

**修改内容**：

1. 添加头文件引用：
```cpp
#include "llvm/CodeGen/SIDSScheduler.h"
```

2. 实现 `createMachineScheduler`：
```cpp
ScheduleDAGInstrs *
WebAssemblyTargetMachine::createMachineScheduler(
    MachineSchedContext *C) const {
  return new ScheduleDAGMI(C, std::make_unique<SIDSScheduler>(), 
                           /*TrackPressure*/ true);
}
```

**原因**：
- `ScheduleDAGMI` 是 LLVM 的 MachineScheduler 实现类
- 通过传入 `std::make_unique<SIDSScheduler>()`，我们的策略被注入
- `TrackPressure=true` 启用寄存器压力跟踪

### 6.3 编译与测试

#### 6.3.1 编译 LLVM

```bash
cd /path/to/llvm-project
mkdir build-assert
cd build-assert

cmake -G Ninja ../llvm \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DLLVM_ENABLE_ASSERTIONS=ON \
  -DLLVM_TARGETS_TO_BUILD="WebAssembly"

ninja llc
```

**关键参数**：
- `RelWithDebInfo`：优化 + 调试信息
- `LLVM_ENABLE_ASSERTIONS=ON`：启用 `-debug-only` 支持
- `LLVM_TARGETS_TO_BUILD="WebAssembly"`：只编译 wasm 目标（加快编译）

#### 6.3.2 验证调度器工作

```bash
# 编译测试文件
./bin/llc -mtriple=wasm32-unknown-unknown \
  -enable-misched \
  -misched=sids \
  -debug -debug-only=sids-scheduler \
  test.ll -o test.s 2> sids.log

# 检查日志
grep "SIDS picked" sids.log
```

**预期输出**：
```
SIDS picked: SU#0 Priority=0.800 StaticW=0.900 ...
SIDS picked: SU#3 Priority=0.750 StaticW=0.850 ...
...
```

---

## 7. 参数调优

### 7.1 命令行参数

所有参数都通过 `cl::opt` 暴露，可在 `llc` 命令行中使用：

| 参数 | 默认值 | 说明 | 推荐范围 |
|------|--------|------|----------|
| `-sids-alpha` | 0.4 | 数据复用度权重 | 0.3 - 0.8 |
| `-sids-beta` | 0.4 | 热点路径权重 | 0.2 - 0.5 |
| `-sids-gamma` | 0.2 | 控制距离惩罚 | 0.1 - 0.3 |
| `-sids-miss-penalty` | 0.5 | Cache miss 惩罚 | 0.3 - 1.5 |
| `-sids-miss-penalty-adaptive` | false | 启用自适应 miss 惩罚 | - |
| `-sids-miss-penalty-beta` | 0.2 | 自适应学习率 | 0.1 - 0.5 |
| `-sids-miss-penalty-window` | 32 | 自适应窗口大小 | 16 - 128 |
| `-sids-ctrl-improved` | false | 启用 CFG BFS 控制距离 | - |

### 7.2 场景化调优建议

#### 7.2.1 计算密集型（密码学、哈希）

```bash
-sids-alpha=0.7 \
-sids-beta=0.2 \
-sids-gamma=0.1 \
-sids-miss-penalty=1.5
```

**理由**：
- 高 Alpha：强化数据复用，让依赖链紧密
- 高 MissPenalty：激进推迟内存访问
- 低 Gamma：减少控制流惩罚

#### 7.2.2 内存密集型（图算法、数据库）

```bash
-sids-alpha=0.3 \
-sids-beta=0.5 \
-sids-gamma=0.2 \
-sids-miss-penalty=0.3
```

**理由**：
- 低 Alpha：内存访问模式不规则，复用度低
- 高 Beta：重视热点路径
- 低 MissPenalty：不过度推迟内存访问

#### 7.2.3 混合负载（通用合约）

```bash
-sids-alpha=0.5 \
-sids-beta=0.3 \
-sids-gamma=0.2 \
-sids-miss-penalty=0.8
```

**理由**：平衡各项权重

---

## 附录

### A. 完整文件列表

详见各分册：
- [SIDS_MANUAL_PART1_HEADER.md](./SIDS_MANUAL_PART1_HEADER.md) - 头文件完整代码
- [SIDS_MANUAL_PART2_IMPL.md](./SIDS_MANUAL_PART2_IMPL.md) - 实现文件完整代码
- [SIDS_MANUAL_PART3_BACKEND.md](./SIDS_MANUAL_PART3_BACKEND.md) - 后端集成代码
- [SIDS_MANUAL_PART4_BUILD.md](./SIDS_MANUAL_PART4_BUILD.md) - 构建配置

### B. 常见问题

见 [SIDS_FAQ.md](./SIDS_FAQ.md)

### C. 性能测试

见 [PAPER_EXPERIMENT_REPORT.md](./PAPER_EXPERIMENT_REPORT.md)
