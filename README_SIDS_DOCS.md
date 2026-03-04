# SIDS 调度器 LLVM 集成完整文档索引

## 📚 文档概览

本文档集详细说明了如何在 LLVM 中实现和集成基于"最短指令距离"思想的 SIDS 调度器。

---

## 📖 主要文档

### 1. [集成手册（主文档）](./SIDS_INTEGRATION_MANUAL.md)
**内容**：
- 概述与架构
- 核心文件清单
- 数据结构设计
- 算法实现概览
- 参数调优指南

**适合**：想快速了解整体架构的读者

---

### 2. [第一部分：头文件设计](./SIDS_MANUAL_PART1_HEADER.md)
**内容**：
- `SIDSScheduler.h` 完整代码
- 每个成员变量的设计理由
- 数据结构选择（为什么用 DenseMap）
- 与 LLVM 编码规范的对应
- 设计模式分析

**适合**：想深入理解类设计的读者

**关键知识点**：
- `DenseMap` vs `std::map` vs `vector`
- 为什么用指针而非引用
- `const` 函数的作用
- 智能指针的使用

---

### 3. [第二部分：核心实现](./SIDS_MANUAL_PART2_IMPL.md)
**内容**：
- `SIDSScheduler.cpp` 完整代码解析
- `initialize()` 算法详解
  - Reuse 计算（1/(平均距离+1)）
  - Hot 计算（基于 Depth）
  - Ctrl 计算（NodeNum 归一化）
- `computeSIDSPriority()` 算法详解
  - 继承前驱权重
  - w·ℓ 距离惩罚
  - Cache miss 惩罚
  - CFG BFS 控制距离
- `pickNode()` 实现
- `schedNode()` 实现

**适合**：想理解算法细节的读者

**关键知识点**：
- 如何用 Depth 差值近似"定义-使用距离"
- 为什么用 `-∞` 作为初始优先级
- 自适应 miss 惩罚的 EMA 算法
- MBB 距离缓存的必要性

---

### 4. [第三部分：后端集成](./SIDS_MANUAL_PART3_BACKEND.md)
**内容**：
- 修改 `WebAssemblyTargetMachine.h`
- 修改 `WebAssemblyTargetMachine.cpp`
- `enableMachineScheduler()` 的作用
- `createMachineScheduler()` 的实现
- 为什么 `TrackPressure=true`
- 如何为其他后端启用 SIDS

**适合**：想将 SIDS 集成到特定后端的读者

**关键知识点**：
- `ScheduleDAGMI` 的参数含义
- `MachineSchedRegistry` 注册机制
- 命令行参数的优先级

---

### 5. [第四部分：构建配置](./SIDS_MANUAL_PART4_BUILD.md)
**内容**：
- 修改 `CMakeLists.txt`
- 完整的编译流程
- 测试与验证方法
- 常见编译问题及解决
- 调试技巧
- 性能分析方法
- 持续集成脚本

**适合**：想实际编译和测试的读者

**关键知识点**：
- CMake 参数详解
- `-debug-only` 的使用
- GDB 调试技巧
- 回归测试方法

---

## 🧪 实验与测试文档

### 6. [合约测试报告](./SIDS_CONTRACT_TEST_REPORT.md)
**内容**：
- 真实合约场景测试
- 编译期调度行为分析
- 热点循环调度对比
- 关键改进点验证

**数据**：
- LOAD 指令推后（第10位 → 第12位）
- 循环条件提前（第9位 → 第8位）
- 高复用指令权重提升 4.9%

---

### 7. [运行时性能报告](./SIDS_RUNTIME_PERFORMANCE_REPORT.md)
**内容**：
- Wasmtime/Wasmer 性能测试
- 为什么 JIT 掩盖了优化效果
- 解释器模式 vs JIT 模式
- 性能提升的局限性分析

**结论**：
- JIT 模式下性能持平
- 需要在 AOT/解释器模式下测试

---

### 8. [论文实验报告](./PAPER_EXPERIMENT_REPORT.md)
**内容**：
- 完整的实验设计
- 三种调度策略对比
- 性能测试结果（1.25% - 3.2% 提升）
- 编译期调度行为验证
- 结果分析与讨论
- 论文写作建议

**适合**：写论文的读者

**关键数据**：
- Wasmtime: 2.3-3.2% 提升
- Wasmer: 1.25-2.5% 提升
- 对照组实验验证了调度策略的重要性

---

## 🗂️ 文件清单

### 新增文件（需要创建）

| 文件 | 位置 | 行数 | 作用 |
|------|------|------|------|
| `SIDSScheduler.h` | `llvm/include/llvm/CodeGen/` | ~80 | 类定义 |
| `SIDSScheduler.cpp` | `llvm/lib/CodeGen/` | ~363 | 实现 |

### 修改文件

| 文件 | 位置 | 修改内容 |
|------|------|----------|
| `CMakeLists.txt` | `llvm/lib/CodeGen/` | 添加 `SIDSScheduler.cpp` |
| `WebAssemblyTargetMachine.h` | `llvm/lib/Target/WebAssembly/` | 声明 `createMachineScheduler()` |
| `WebAssemblyTargetMachine.cpp` | `llvm/lib/Target/WebAssembly/` | 实现 `createMachineScheduler()` |

---

## 🎯 快速开始指南

### 对于急于上手的读者

1. **创建头文件**
   - 复制 [第一部分](./SIDS_MANUAL_PART1_HEADER.md) 中的完整代码
   - 保存为 `llvm/include/llvm/CodeGen/SIDSScheduler.h`

2. **创建实现文件**
   - 复制 [第二部分](./SIDS_MANUAL_PART2_IMPL.md) 中的完整代码
   - 保存为 `llvm/lib/CodeGen/SIDSScheduler.cpp`

3. **修改构建配置**
   - 按 [第四部分](./SIDS_MANUAL_PART4_BUILD.md) 修改 `CMakeLists.txt`

4. **集成到后端**
   - 按 [第三部分](./SIDS_MANUAL_PART3_BACKEND.md) 修改 WebAssembly 后端

5. **编译测试**
   ```bash
   mkdir build && cd build
   cmake -G Ninja ../llvm \
     -DCMAKE_BUILD_TYPE=RelWithDebInfo \
     -DLLVM_ENABLE_ASSERTIONS=ON \
     -DLLVM_TARGETS_TO_BUILD="WebAssembly"
   ninja llc
   ```

6. **验证功能**
   ```bash
   ./bin/llc -mtriple=wasm32-unknown-unknown \
     -enable-misched -misched=sids \
     -debug -debug-only=sids-scheduler \
     test.ll -o test.s 2> sids.log
   ```

---

## 📊 核心算法总结

### 静态权重计算

```
W(u) = α·Reuse(u) + β·Hot(u) - γ·Ctrl(u)

其中：
- Reuse(u) = 1 / (平均定义-使用距离 + 1)
- Hot(u) = 1 + Depth(u) * 0.1
- Ctrl(u) = NodeNum(u) / |V|
```

### 动态优先级计算

```
Priority(u) = max{W(pred) | pred ∈ DataPreds(u)}
            - DistanceScale * Σ W(pred) * ℓ(pred, u)
            - MissPenalty (if u is load/store)
            - Γ * CtrlDist(u) (if improved ctrl enabled)
```

### 目标函数

```
min Σ w(u,v) · ℓ(u,v)
```

---

## 🔧 关键设计决策

### 1. 数据结构选择

| 用途 | 选择 | 理由 |
|------|------|------|
| 权重缓存 | `DenseMap<SUnit*, float>` | O(1) 查找，内存紧凑 |
| 调度顺序 | `DenseMap<SUnit*, unsigned>` | 支持距离计算 |
| MBB 距离 | 两层 `DenseMap` | 缓存 BFS 结果 |

### 2. 算法选择

| 问题 | 方案 | 理由 |
|------|------|------|
| 复用度估算 | Depth 差值 | 简单有效，LLVM 已提供 |
| 热点估算 | Depth 加成 | 关键路径代理 |
| 控制距离 | NodeNum 归一化 | 快速近似 |
| 节点选择 | 遍历所有 SUnit | 简化实现 |

### 3. 参数默认值

| 参数 | 默认值 | 理由 |
|------|--------|------|
| Alpha | 0.4 | 平衡复用度和热点 |
| Beta | 0.4 | 平衡复用度和热点 |
| Gamma | 0.2 | 控制距离惩罚较小 |
| MissPenalty | 0.5 | 适度推迟内存访问 |

---

## 🐛 常见问题速查

### 编译问题

| 错误 | 原因 | 解决方案 |
|------|------|----------|
| `undefined reference` | 未加入 CMakeLists.txt | 添加 `SIDSScheduler.cpp` |
| `file not found` | 头文件路径错误 | 检查 `#include` 路径 |
| `multiple definition` | 函数在头文件中定义 | 移到 .cpp 文件 |

### 运行时问题

| 错误 | 原因 | 解决方案 |
|------|------|----------|
| `Nonempty unscheduled zone` | `pickNode()` 返回 `nullptr` | 使用 `-∞` 初始优先级 |
| `-debug-only` 无输出 | 未启用 assertions | 重编译 `-DLLVM_ENABLE_ASSERTIONS=ON` |
| 参数不生效 | 拼写错误 | 检查 `-sids-alpha` 等 |

---

## 📈 性能优化建议

### 编译期优化

1. **使用 ccache**
   ```bash
   -DCMAKE_C_COMPILER_LAUNCHER=ccache
   ```

2. **并行编译**
   ```bash
   ninja -j$(nproc)
   ```

3. **只编译需要的目标**
   ```bash
   -DLLVM_TARGETS_TO_BUILD="WebAssembly"
   ```

### 运行时优化

1. **调整参数**
   - 计算密集：`-sids-alpha=0.7 -sids-miss-penalty=1.5`
   - 内存密集：`-sids-alpha=0.3 -sids-miss-penalty=0.3`

2. **启用改进版控制距离**
   ```bash
   -sids-ctrl-improved
   ```

3. **自适应 miss 惩罚**
   ```bash
   -sids-miss-penalty-adaptive
   ```

---

## 🎓 学习路径建议

### 初学者
1. 阅读 [主手册](./SIDS_INTEGRATION_MANUAL.md) 了解架构
2. 阅读 [第一部分](./SIDS_MANUAL_PART1_HEADER.md) 理解类设计
3. 按 [快速开始](#快速开始指南) 实际操作

### 进阶者
1. 深入阅读 [第二部分](./SIDS_MANUAL_PART2_IMPL.md) 理解算法
2. 阅读 [第三部分](./SIDS_MANUAL_PART3_BACKEND.md) 学习集成
3. 阅读 [论文实验报告](./PAPER_EXPERIMENT_REPORT.md) 了解性能

### 研究者
1. 阅读所有技术文档
2. 研究 [合约测试报告](./SIDS_CONTRACT_TEST_REPORT.md) 中的调度行为
3. 尝试改进算法（如接入 PGO）

---

## 📞 支持与反馈

### 文档问题
- 如果发现文档错误或不清楚的地方，请提 issue

### 技术问题
- 编译问题：查看 [第四部分](./SIDS_MANUAL_PART4_BUILD.md)
- 算法问题：查看 [第二部分](./SIDS_MANUAL_PART2_IMPL.md)
- 性能问题：查看 [论文实验报告](./PAPER_EXPERIMENT_REPORT.md)

---

## 📝 版本历史

- **v1.0** (2024-03-03)
  - 初始版本
  - 完整的 SIDS 调度器实现
  - WebAssembly 后端集成
  - 性能测试与验证

---

## 📄 许可证

本文档集与 LLVM 项目使用相同的许可证：Apache License 2.0 with LLVM Exceptions

---

## 🙏 致谢

感谢 LLVM 社区提供的优秀框架和文档。
