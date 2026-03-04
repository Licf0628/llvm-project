# SIDS 集成手册 - 第一部分：头文件设计

## 文件信息

- **文件路径**：`llvm/include/llvm/CodeGen/SIDSScheduler.h`
- **作用**：定义 SIDS 调度器类的接口
- **代码行数**：约 80 行

---

## 完整代码

```cpp
//===- SIDSScheduler.h - Shortest Instruction Distance Scheduler -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the SIDSScheduler class, which implements a machine
// instruction scheduler based on the "Shortest Instruction Distance" principle.
//
// The scheduler aims to minimize the weighted instruction distance:
//   min Σ w(u,v) · ℓ(u,v)
// where:
//   - w(u,v) is the edge weight (based on reuse, hotness, control distance)
//   - ℓ(u,v) is the distance between instructions u and v in the schedule
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_SIDSSCHEDULER_H
#define LLVM_CODEGEN_SIDSSCHEDULER_H

#include "llvm/CodeGen/MachineScheduler.h"
#include "llvm/ADT/DenseMap.h"

namespace llvm {

/// SIDSScheduler - Shortest Instruction Distance Scheduler
///
/// This scheduler implements a strategy that minimizes the weighted distance
/// between dependent instructions, considering:
///   1. Data reuse (based on def-use distance)
///   2. Hotness (based on critical path depth)
///   3. Control distance (based on CFG structure)
///   4. Cache miss penalty (for load/store instructions)
class SIDSScheduler : public MachineSchedStrategy {
private:
  /// Pointer to the scheduling DAG
  ScheduleDAGMI *DAG = nullptr;
  
  /// Static weight for each SUnit, computed during initialization
  /// Key: SUnit pointer, Value: combined weight (Alpha*Reuse + Beta*Hot - Gamma*Ctrl)
  DenseMap<const SUnit *, float> StaticWeight;

  /// Weight parameters (configurable via command-line options)
  float Alpha = 0.4f;  ///< Weight for data reuse
  float Beta  = 0.4f;  ///< Weight for hotness
  float Gamma = 0.2f;  ///< Penalty for control distance
  
  /// Scale applied when an SUnit is a load/store to penalize potential misses
  float MissPenaltyScale = 0.5f;
  
  /// Adaptive miss-penalty support (simulation-based)
  bool AdaptiveMissPenalty = false;
  float AdaptiveBeta = 0.2f;       ///< Smoothing/learning rate
  unsigned AdaptiveWindow = 32;    ///< Number of scheduled nodes per update window
  unsigned AdaptiveLoadCount = 0;  ///< Counter for loads scheduled in current window
  
  /// Approximate "instruction distance" ℓ by tracking the order in which
  /// SUnits are scheduled
  DenseMap<const SUnit *, unsigned> ScheduledOrder;
  unsigned ScheduledCount = 0;
  
  /// Cache for MBB-to-MBB distances (used in improved control distance metric)
  DenseMap<const MachineBasicBlock*, 
           DenseMap<const MachineBasicBlock*, unsigned>> MBBDistCache;

  /// Compute the SIDS priority for a given SUnit
  /// This combines static weight, distance cost, and miss penalty
  float computeSIDSPriority(const SUnit *SU) const;

public:
  SIDSScheduler() = default;

  /// Initialize the scheduler for a new scheduling region
  void initialize(ScheduleDAGMI *dag) override;

  /// Pick the next node to schedule from the ready queue
  SUnit *pickNode(bool &IsTopNode) override;

  /// Notify the scheduler that a node has been scheduled
  void schedNode(SUnit *SU, bool IsTopNode) override;

  /// Release successors of a scheduled node (called by ScheduleDAGMI)
  void releaseTopNode(SUnit *SU) override {}

  /// Release predecessors of a scheduled node (called by ScheduleDAGMI)
  void releaseBottomNode(SUnit *SU) override {}
};

/// Factory function to create a SIDS scheduler instance
std::unique_ptr<MachineSchedStrategy> createSIDSScheduler();

} // end namespace llvm

#endif // LLVM_CODEGEN_SIDSSCHEDULER_H
```

---

## 逐段解析

### 1. 头文件保护与命名空间

```cpp
#ifndef LLVM_CODEGEN_SIDSSCHEDULER_H
#define LLVM_CODEGEN_SIDSSCHEDULER_H
```

**作用**：防止头文件被重复包含

**命名规范**：
- LLVM 使用 `LLVM_<目录>_<文件名>_H` 格式
- 全大写，路径用下划线分隔

### 2. 包含依赖

```cpp
#include "llvm/CodeGen/MachineScheduler.h"
#include "llvm/ADT/DenseMap.h"
```

**为什么需要这些头文件**：

1. **MachineScheduler.h**
   - 提供 `MachineSchedStrategy` 基类
   - 提供 `ScheduleDAGMI` 类定义
   - 提供 `SUnit` 结构体定义

2. **DenseMap.h**
   - LLVM 的高效哈希表实现
   - 用于存储 `StaticWeight` 和 `ScheduledOrder`

**为什么不包含其他头文件**：
- 头文件应该最小化依赖
- 其他需要的类型（如 `MachineBasicBlock`）通过前向声明或在 .cpp 中包含

### 3. 类定义

```cpp
class SIDSScheduler : public MachineSchedStrategy {
```

**继承关系**：
- `MachineSchedStrategy` 是 LLVM 调度策略的抽象接口
- 必须实现的虚函数：
  - `initialize()`
  - `pickNode()`
  - `schedNode()`
  - `releaseTopNode()` / `releaseBottomNode()`

### 4. 私有成员变量

#### 4.1 调度上下文

```cpp
ScheduleDAGMI *DAG = nullptr;
```

**作用**：指向当前调度区域的 DAG

**使用场景**：
- 在 `initialize()` 中被赋值
- 在 `pickNode()` 中遍历 `DAG->SUnits`
- 在 `computeSIDSPriority()` 中访问依赖信息

#### 4.2 静态权重缓存

```cpp
DenseMap<const SUnit *, float> StaticWeight;
```

**数据结构选择**：`DenseMap`

**为什么用 DenseMap**：
- **优点**：
  - O(1) 平均查找时间
  - 内存紧凑（相比 `std::unordered_map`）
  - LLVM 标准容器，与其他代码风格一致
- **替代方案**：
  - `std::vector<float>`：需要 SUnit 有连续 ID
  - `std::map`：O(log n) 查找，较慢

**Key 类型**：`const SUnit *`
- 使用指针作为 key
- `const` 表示不修改 SUnit 本身

**Value 类型**：`float`
- 权重是浮点数
- 范围通常在 0.0 - 2.0

#### 4.3 权重参数

```cpp
float Alpha = 0.4f;
float Beta  = 0.4f;
float Gamma = 0.2f;
```

**设计决策**：

1. **为什么用成员变量而非常量**：
   - 需要从命令行参数动态设置
   - 在 `initialize()` 中从 `cl::opt` 读取

2. **默认值选择**：
   - Alpha=0.4, Beta=0.4：平衡复用度和热点
   - Gamma=0.2：控制距离惩罚相对较小
   - 基于实验调优得出

3. **为什么用 float 而非 double**：
   - 精度足够（权重计算不需要高精度）
   - 节省内存

#### 4.4 调度顺序跟踪

```cpp
DenseMap<const SUnit *, unsigned> ScheduledOrder;
unsigned ScheduledCount = 0;
```

**作用**：实现 w·ℓ 距离计算

**工作流程**：
1. `schedNode()` 被调用时：`ScheduledOrder[SU] = ScheduledCount++`
2. `computeSIDSPriority()` 中：`Dist = CurIndex - ScheduledOrder[Pred]`

**为什么需要**：
- 文档中的目标函数是 `min Σ w·ℓ`
- 必须知道每个节点的调度位置才能计算距离

#### 4.5 MBB 距离缓存

```cpp
DenseMap<const MachineBasicBlock*, 
         DenseMap<const MachineBasicBlock*, unsigned>> MBBDistCache;
```

**数据结构**：两层嵌套 DenseMap

**为什么需要缓存**：
- CFG 上的 BFS 计算开销大（O(V+E)）
- 同一对 MBB 可能被多次查询
- 缓存后变成 O(1) 查找

**结构**：
```
MBBDistCache[SrcMBB][DstMBB] = distance
```

**使用场景**：
- 仅在 `-sids-ctrl-improved` 启用时使用
- 在 `computeSIDSPriority()` 中计算控制依赖惩罚

### 5. 私有成员函数

```cpp
float computeSIDSPriority(const SUnit *SU) const;
```

**为什么是私有**：
- 仅在 `pickNode()` 内部使用
- 不需要暴露给外部

**为什么是 const**：
- 不修改 `SIDSScheduler` 的状态
- 只读取 `StaticWeight`、`ScheduledOrder` 等

**返回值**：`float`
- 优先级可以是负数（被 miss penalty 压低）
- 范围通常在 -2.0 到 2.0

### 6. 公有接口

#### 6.1 构造函数

```cpp
SIDSScheduler() = default;
```

**为什么用 default**：
- 所有成员变量都有默认初始化值
- 不需要自定义构造逻辑

#### 6.2 核心虚函数

```cpp
void initialize(ScheduleDAGMI *dag) override;
SUnit *pickNode(bool &IsTopNode) override;
void schedNode(SUnit *SU, bool IsTopNode) override;
```

**override 关键字**：
- C++11 特性
- 明确表示覆盖基类虚函数
- 编译器会检查签名是否匹配

#### 6.3 空实现的虚函数

```cpp
void releaseTopNode(SUnit *SU) override {}
void releaseBottomNode(SUnit *SU) override {}
```

**为什么是空实现**：
- SIDS 调度器不需要手动管理就绪队列
- 在 `pickNode()` 中直接遍历所有 SUnit
- 这些函数由 `ScheduleDAGMI` 调用，但我们不需要做任何事

### 7. 工厂函数

```cpp
std::unique_ptr<MachineSchedStrategy> createSIDSScheduler();
```

**作用**：创建 SIDS 调度器实例

**为什么需要**：
- 用于 `MachineSchedRegistry` 注册
- 返回智能指针，自动管理生命周期

**实现位置**：在 `SIDSScheduler.cpp` 中

---

## 设计模式

### 1. 策略模式（Strategy Pattern）

```
MachineSchedStrategy (接口)
        ↑
        | 实现
        |
   SIDSScheduler
```

**优点**：
- 可插拔的调度策略
- 不修改 `ScheduleDAGMI` 就能添加新策略

### 2. 缓存模式（Cache Pattern）

```cpp
DenseMap<const SUnit *, float> StaticWeight;  // 缓存静态权重
DenseMap<...> MBBDistCache;                   // 缓存 MBB 距离
```

**优点**：
- 避免重复计算
- 空间换时间

### 3. 模板方法模式（Template Method）

```
ScheduleDAGMI::schedule() {
    strategy->initialize(this);
    while (有未调度节点) {
        SU = strategy->pickNode(...);
        strategy->schedNode(SU, ...);
    }
}
```

**优点**：
- 框架控制调度流程
- 策略只需实现关键步骤

---

## 与 LLVM 编码规范的对应

### 1. 命名规范

- **类名**：`SIDSScheduler`（大驼峰）
- **成员变量**：`StaticWeight`（大驼峰）
- **成员函数**：`pickNode`（小驼峰）
- **局部变量**：`dag`（小写）

### 2. 注释规范

```cpp
/// Brief description
///
/// Detailed description
/// with multiple lines
```

- 使用 `///` 而非 `//`（Doxygen 格式）
- 简短描述 + 详细描述

### 3. 头文件组织

```
1. 版权声明
2. 文件说明
3. 头文件保护
4. 包含依赖
5. 命名空间
6. 类定义
7. 工厂函数
8. 命名空间结束
9. 头文件保护结束
```

---

## 常见问题

### Q1: 为什么不用 `std::map` 而用 `DenseMap`？

**A**: LLVM 的 `DenseMap` 针对指针 key 优化，性能更好：
- 内存布局紧凑
- 缓存友好
- 与 LLVM 其他代码一致

### Q2: 为什么 `DAG` 是指针而非引用？

**A**: 
- `initialize()` 中才赋值，构造时还不知道
- 指针可以为 `nullptr`，方便检查是否已初始化

### Q3: 为什么权重参数不是 `const`？

**A**: 需要从命令行参数动态设置：
```cpp
Alpha = SIDSAlpha;  // SIDSAlpha 是 cl::opt
```

### Q4: 为什么 `ScheduledOrder` 用 `unsigned` 而非 `int`？

**A**: 
- 调度序号不会是负数
- `unsigned` 范围更大（0 到 4B）
- 与 LLVM 其他序号类型一致

---

## 下一步

继续阅读：
- [第二部分：核心实现](./SIDS_MANUAL_PART2_IMPL.md)
- [第三部分：后端集成](./SIDS_MANUAL_PART3_BACKEND.md)
