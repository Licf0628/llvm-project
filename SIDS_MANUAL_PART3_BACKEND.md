# SIDS 集成手册 - 第三部分：后端集成

## 概述

本部分说明如何将 SIDS 调度器集成到 WebAssembly 后端，使其成为 wasm 目标的默认调度器。

---

## 1. 修改 WebAssemblyTargetMachine.h

### 1.1 文件路径

```
llvm/lib/Target/WebAssembly/WebAssemblyTargetMachine.h
```

### 1.2 修改内容

在 `WebAssemblyTargetMachine` 类中添加两个方法：

```cpp
class WebAssemblyTargetMachine final : public LLVMTargetMachine {
  // ... 现有成员 ...
  
public:
  // ... 现有方法 ...
  
  /// 启用 MachineScheduler pass
  bool enableMachineScheduler() const override { return true; }
  
  /// 创建自定义的 MachineScheduler 实例
  ScheduleDAGInstrs *
  createMachineScheduler(MachineSchedContext *C) const override;
};
```

### 1.3 详细解析

#### 1.3.1 enableMachineScheduler()

```cpp
bool enableMachineScheduler() const override { return true; }
```

**作用**：
- 告诉 LLVM 为 WebAssembly 目标启用 MachineScheduler pass
- 默认情况下，某些目标不启用调度器

**为什么需要**：
- WebAssembly 后端默认可能不启用调度
- 必须显式返回 `true` 才能触发调度 pass

**override 关键字**：
- 表示覆盖基类 `LLVMTargetMachine` 的虚函数
- 编译器会检查签名是否匹配

#### 1.3.2 createMachineScheduler()

```cpp
ScheduleDAGInstrs *
createMachineScheduler(MachineSchedContext *C) const override;
```

**作用**：
- 创建自定义的调度器实例
- 返回 `ScheduleDAGInstrs` 指针（基类）

**参数**：
- `MachineSchedContext *C`：调度上下文
  - 包含 `MachineFunction`、`MachineLoopInfo` 等信息
  - 由 LLVM 框架传入

**返回值**：
- `ScheduleDAGInstrs *`：调度 DAG 实例
- 实际返回 `ScheduleDAGMI`（派生类）

**为什么是 const**：
- 不修改 `TargetMachine` 的状态
- 只是创建新对象

---

## 2. 修改 WebAssemblyTargetMachine.cpp

### 2.1 文件路径

```
llvm/lib/Target/WebAssembly/WebAssemblyTargetMachine.cpp
```

### 2.2 添加头文件引用

在文件开头添加：

```cpp
#include "llvm/CodeGen/SIDSScheduler.h"
```

**位置**：
- 在其他 `#include` 语句之后
- 在 `using namespace llvm;` 之前

**为什么需要**：
- 使用 `SIDSScheduler` 类
- 调用 `std::make_unique<SIDSScheduler>()`

### 2.3 实现 createMachineScheduler()

在文件中添加方法实现：

```cpp
ScheduleDAGInstrs *
WebAssemblyTargetMachine::createMachineScheduler(
    MachineSchedContext *C) const {
  return new ScheduleDAGMI(C, std::make_unique<SIDSScheduler>(), 
                           /*TrackPressure*/ true);
}
```

**位置建议**：
- 在其他 `WebAssemblyTargetMachine` 方法附近
- 通常在构造函数之后

### 2.4 详细解析

#### 2.4.1 ScheduleDAGMI 类

```cpp
ScheduleDAGMI(MachineSchedContext *C, 
              std::unique_ptr<MachineSchedStrategy> S,
              bool TrackPressure)
```

**参数说明**：

1. **MachineSchedContext *C**
   - 调度上下文，由框架传入
   - 包含函数、循环、寄存器信息等

2. **std::unique_ptr<MachineSchedStrategy> S**
   - 调度策略（我们的 SIDS 调度器）
   - 使用智能指针，自动管理生命周期

3. **bool TrackPressure**
   - 是否跟踪寄存器压力
   - `true`：启用寄存器压力跟踪
   - `false`：禁用（可能导致寄存器溢出）

#### 2.4.2 为什么用 std::make_unique？

```cpp
std::make_unique<SIDSScheduler>()
```

**优点**：
- 异常安全
- 代码简洁
- C++14 标准

**等价写法**（C++11）：
```cpp
std::unique_ptr<SIDSScheduler>(new SIDSScheduler())
```

#### 2.4.3 为什么 TrackPressure=true？

**原因**：
- 寄存器压力跟踪能避免寄存器溢出
- 对于 WebAssembly 这种寄存器数量有限的目标很重要
- 性能影响可接受

**如果设为 false**：
- 调度器可能生成需要过多寄存器的代码
- 导致寄存器分配器插入 spill/reload 指令
- 性能下降

#### 2.4.4 为什么用 new 而非 make_unique？

```cpp
return new ScheduleDAGMI(...);
```

**原因**：
- 返回类型是裸指针 `ScheduleDAGInstrs *`
- LLVM 框架会负责释放内存
- 不能返回 `unique_ptr`（类型不匹配）

**内存管理**：
- LLVM 的 pass 管理器会在适当时机 `delete`
- 不会内存泄漏

---

## 3. 集成效果

### 3.1 调度器选择逻辑

```
编译 wasm 目标时：
    ↓
enableMachineScheduler() 返回 true
    ↓
LLVM 调用 createMachineScheduler()
    ↓
返回 ScheduleDAGMI(带 SIDS 策略)
    ↓
使用 SIDS 调度器进行指令调度
```

### 3.2 命令行控制

#### 3.2.1 使用默认 SIDS 调度器

```bash
llc -mtriple=wasm32-unknown-unknown test.ll -o test.s
```

- 自动使用 SIDS 调度器
- 无需额外参数

#### 3.2.2 显式指定调度器

```bash
llc -mtriple=wasm32-unknown-unknown -misched=sids test.ll -o test.s
```

- 显式指定使用 SIDS
- 与默认行为相同

#### 3.2.3 使用其他调度器

```bash
llc -mtriple=wasm32-unknown-unknown -misched=default test.ll -o test.s
```

- 覆盖默认行为
- 使用 LLVM 内置的默认调度器

#### 3.2.4 禁用调度器

```bash
llc -mtriple=wasm32-unknown-unknown -enable-misched=false test.ll -o test.s
```

- 完全禁用 MachineScheduler
- 使用源顺序

---

## 4. 其他后端集成

### 4.1 为其他目标启用 SIDS

如果想为 x86、ARM 等目标启用 SIDS，可以：

#### 方法 1：修改对应的 TargetMachine

类似 WebAssembly 的做法，修改：
- `X86TargetMachine.h` / `X86TargetMachine.cpp`
- `ARMTargetMachine.h` / `ARMTargetMachine.cpp`

#### 方法 2：使用命令行参数

不修改代码，直接用 `-misched=sids`：

```bash
llc -mtriple=x86_64-unknown-linux-gnu -misched=sids test.ll -o test.s
```

**前提**：
- 目标已经启用了 MachineScheduler
- SIDS 已经通过 `MachineSchedRegistry` 注册

### 4.2 注册机制的优势

```cpp
static MachineSchedRegistry
SIDSRegistry("sids", "SIDS scheduler", createSIDSScheduler);
```

**优点**：
- 全局注册，所有目标都能用
- 通过 `-misched=sids` 启用
- 无需修改每个后端

**局限**：
- 不是默认调度器
- 需要显式指定

---

## 5. 验证集成

### 5.1 检查调度器是否生效

```bash
llc -mtriple=wasm32-unknown-unknown \
    -enable-misched \
    -debug -debug-only=sids-scheduler \
    test.ll -o test.s 2> sids.log
```

**预期输出**（sids.log）：
```
=== SIDS Scheduler initialized ===
  Alpha=0.400000 Beta=0.400000 Gamma=0.200000 MissPenalty=0.500000
  Total SUnits: 12
SIDS picked: SU#9 Priority=0.906667 StaticW=1.130000 ...
SIDS picked: SU#8 Priority=0.883333 StaticW=0.906666 ...
...
```

### 5.2 对比不同调度器

```bash
# 默认调度器
llc -mtriple=wasm32-unknown-unknown -misched=default test.ll -o default.s

# SIDS 调度器
llc -mtriple=wasm32-unknown-unknown -misched=sids test.ll -o sids.s

# 对比
diff default.s sids.s
```

---

## 6. 常见问题

### Q1: 为什么 wasm 默认使用 SIDS，而 x86 不是？

**A**: 
- 我们只修改了 `WebAssemblyTargetMachine`
- 其他后端保持原样
- 可以通过 `-misched=sids` 手动启用

### Q2: 如何让 SIDS 成为所有目标的默认调度器？

**A**: 修改 LLVM 核心代码（不推荐）：
```cpp
// llvm/lib/CodeGen/MachineScheduler.cpp
std::unique_ptr<MachineSchedStrategy>
createDefaultMachineScheduler(...) {
  return std::make_unique<SIDSScheduler>();  // 替换默认策略
}
```

### Q3: TrackPressure 对性能有多大影响？

**A**: 
- 编译时间增加约 5-10%
- 生成代码质量提升（减少 spill）
- 通常值得开启

### Q4: 能否在运行时动态切换调度器？

**A**: 
- 不能，调度器在编译时确定
- 但可以通过命令行参数选择不同策略

---

## 7. 集成检查清单

- [ ] 修改 `WebAssemblyTargetMachine.h`
  - [ ] 添加 `enableMachineScheduler()`
  - [ ] 声明 `createMachineScheduler()`
  
- [ ] 修改 `WebAssemblyTargetMachine.cpp`
  - [ ] 包含 `SIDSScheduler.h`
  - [ ] 实现 `createMachineScheduler()`
  
- [ ] 编译 LLVM
  - [ ] 无编译错误
  - [ ] 链接成功
  
- [ ] 验证功能
  - [ ] 运行测试用例
  - [ ] 检查调试日志
  - [ ] 对比生成代码

---

## 下一步

继续阅读：
- [第四部分：构建配置](./SIDS_MANUAL_PART4_BUILD.md)
- [返回主手册](./SIDS_INTEGRATION_MANUAL.md)
