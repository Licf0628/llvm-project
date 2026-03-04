# SIDS 集成手册 - 第四部分：构建配置与测试

## 1. 构建系统集成

### 1.1 修改 CMakeLists.txt

#### 文件路径
```
llvm/lib/CodeGen/CMakeLists.txt
```

#### 修改内容

找到 `add_llvm_component_library(LLVMCodeGen` 部分，在源文件列表中添加：

```cmake
add_llvm_component_library(LLVMCodeGen
  AggressiveAntiDepBreaker.cpp
  AllocationOrder.cpp
  # ... 其他文件 ...
  SIDSScheduler.cpp        # ← 添加这一行
  # ... 其他文件 ...
  )
```

#### 为什么需要修改 CMakeLists.txt？

**CMake 的作用**：
- 管理 LLVM 的构建过程
- 确定哪些源文件需要编译
- 生成 Makefile 或 Ninja 构建文件

**不添加会怎样**：
- `SIDSScheduler.cpp` 不会被编译
- 链接时找不到 `SIDSScheduler` 符号
- 报错：`undefined reference to SIDSScheduler::initialize`

#### 文件位置

在 `CMakeLists.txt` 中，源文件通常按字母顺序排列：

```cmake
# 建议插入位置（S 开头的文件附近）
ScheduleDAGInstrs.cpp
ScheduleDAGPrinter.cpp
ScoreboardHazardRecognizer.cpp
SIDSScheduler.cpp          # ← 这里
ShrinkWrap.cpp
SjLjEHPrepare.cpp
```

### 1.2 验证 CMake 配置

#### 重新配置

```bash
cd /path/to/llvm-project/build
cmake -S ../llvm -B .
```

**预期输出**：
```
-- Configuring done
-- Generating done
-- Build files have been written to: /path/to/build
```

**如果出错**：
- 检查 `SIDSScheduler.cpp` 路径是否正确
- 检查 CMakeLists.txt 语法

#### 检查生成的构建文件

```bash
# Ninja
grep SIDSScheduler build.ninja

# Make
grep SIDSScheduler Makefile
```

**预期输出**：
```
lib/CodeGen/CMakeFiles/LLVMCodeGen.dir/SIDSScheduler.cpp.o
```

---

## 2. 编译 LLVM

### 2.1 完整构建流程

#### 步骤 1：创建构建目录

```bash
cd /path/to/llvm-project
mkdir build-assert
cd build-assert
```

**为什么用单独的构建目录**：
- 源代码和构建产物分离
- 方便清理（直接删除 build 目录）
- 支持多种构建配置（Debug/Release）

#### 步骤 2：配置 CMake

```bash
cmake -G Ninja ../llvm \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DLLVM_ENABLE_ASSERTIONS=ON \
  -DLLVM_TARGETS_TO_BUILD="WebAssembly" \
  -DLLVM_ENABLE_PROJECTS="" \
  -DCMAKE_INSTALL_PREFIX=/path/to/install
```

**参数详解**：

| 参数 | 值 | 说明 |
|------|-----|------|
| `-G` | `Ninja` | 使用 Ninja 构建系统（比 Make 快） |
| `CMAKE_BUILD_TYPE` | `RelWithDebInfo` | 优化 + 调试信息 |
| `LLVM_ENABLE_ASSERTIONS` | `ON` | 启用断言和 `-debug-only` |
| `LLVM_TARGETS_TO_BUILD` | `"WebAssembly"` | 只编译 wasm 目标（加快编译） |
| `LLVM_ENABLE_PROJECTS` | `""` | 不编译其他项目（clang 等） |
| `CMAKE_INSTALL_PREFIX` | `/path/to/install` | 安装路径（可选） |

**其他常用参数**：

```bash
# 编译所有目标
-DLLVM_TARGETS_TO_BUILD="all"

# 同时编译 clang
-DLLVM_ENABLE_PROJECTS="clang"

# Debug 构建（更慢但调试友好）
-DCMAKE_BUILD_TYPE=Debug

# 使用 ccache 加速重编译
-DCMAKE_C_COMPILER_LAUNCHER=ccache \
-DCMAKE_CXX_COMPILER_LAUNCHER=ccache
```

#### 步骤 3：编译

```bash
# 编译 llc（LLVM 静态编译器）
ninja llc

# 或编译所有目标
ninja
```

**编译时间**：
- 只编译 `llc` + WebAssembly：约 10-30 分钟
- 编译所有目标：约 1-3 小时
- 取决于 CPU 核心数和内存

**并行编译**：
```bash
# 使用 8 个并行任务
ninja -j8 llc
```

#### 步骤 4：验证编译结果

```bash
# 检查 llc 是否生成
ls -lh bin/llc

# 检查版本
./bin/llc --version

# 检查支持的目标
./bin/llc --version | grep WebAssembly
```

**预期输出**：
```
LLVM (http://llvm.org/):
  LLVM version 18.0.0git
  Optimized build with assertions.
  Default target: x86_64-unknown-linux-gnu
  Host CPU: skylake

  Registered Targets:
    wasm32 - WebAssembly 32-bit
    wasm64 - WebAssembly 64-bit
```

### 2.2 增量编译

修改代码后，只需重新编译：

```bash
ninja llc
```

**Ninja 的优势**：
- 自动检测修改的文件
- 只重新编译必要的部分
- 比 Make 快 2-5 倍

### 2.3 清理构建

```bash
# 清理所有构建产物
ninja clean

# 完全重新开始
cd ..
rm -rf build-assert
mkdir build-assert
cd build-assert
# 重新运行 cmake ...
```

---

## 3. 测试与验证

### 3.1 基础功能测试

#### 测试 1：编译简单的 IR

创建测试文件 `test.ll`：

```llvm
target triple = "wasm32-unknown-unknown"

define i32 @add(i32 %a, i32 %b) {
  %sum = add i32 %a, %b
  ret i32 %sum
}
```

编译：

```bash
./bin/llc -mtriple=wasm32-unknown-unknown test.ll -o test.s
```

**预期**：无错误，生成 `test.s`

#### 测试 2：启用调试输出

```bash
./bin/llc -mtriple=wasm32-unknown-unknown \
  -enable-misched \
  -debug -debug-only=sids-scheduler \
  test.ll -o test.s 2> sids.log
```

**检查 sids.log**：

```bash
cat sids.log
```

**预期输出**：
```
=== SIDS Scheduler initialized ===
  Alpha=0.400000 Beta=0.400000 Gamma=0.200000 MissPenalty=0.500000
  Total SUnits: 3
SIDS picked: SU#0 Priority=0.000000 StaticW=0.800000 ...
SIDS picked: SU#1 Priority=0.000000 StaticW=0.533333 ...
SIDS picked: SU#2 Priority=0.000000 StaticW=0.666667 ...
```

#### 测试 3：参数调整

```bash
./bin/llc -mtriple=wasm32-unknown-unknown \
  -enable-misched \
  -sids-alpha=0.7 \
  -sids-beta=0.2 \
  -sids-gamma=0.1 \
  -debug -debug-only=sids-scheduler \
  test.ll -o test.s 2> sids_tuned.log
```

**检查参数是否生效**：

```bash
grep "Alpha=" sids_tuned.log
```

**预期**：
```
Alpha=0.700000 Beta=0.200000 Gamma=0.100000
```

### 3.2 回归测试

#### 使用 LLVM 测试套件

```bash
# 运行 CodeGen 测试
ninja check-llvm-codegen

# 只运行 WebAssembly 测试
ninja check-llvm-codegen-webassembly
```

**预期**：
- 所有测试通过
- 如果有失败，检查是否是 SIDS 引入的问题

#### 添加自定义测试

在 `llvm/test/CodeGen/WebAssembly/` 下创建测试文件：

```llvm
; RUN: llc < %s -mtriple=wasm32-unknown-unknown -enable-misched -misched=sids | FileCheck %s

target triple = "wasm32-unknown-unknown"

; CHECK-LABEL: test_sids:
define i32 @test_sids(i32 %a, i32 %b) {
  %sum = add i32 %a, %b
  %mul = mul i32 %sum, 2
  ret i32 %mul
}
```

运行测试：

```bash
./bin/llvm-lit test/CodeGen/WebAssembly/test_sids.ll
```

### 3.3 性能测试

见 [PAPER_EXPERIMENT_REPORT.md](./PAPER_EXPERIMENT_REPORT.md)

---

## 4. 常见编译问题

### 4.1 链接错误

**错误信息**：
```
undefined reference to `llvm::SIDSScheduler::initialize(...)`
```

**原因**：
- `SIDSScheduler.cpp` 未加入 CMakeLists.txt
- 或未重新运行 cmake

**解决**：
```bash
# 1. 检查 CMakeLists.txt
grep SIDSScheduler llvm/lib/CodeGen/CMakeLists.txt

# 2. 重新配置
cd build
cmake -S ../llvm -B .

# 3. 重新编译
ninja llc
```

### 4.2 头文件找不到

**错误信息**：
```
fatal error: 'llvm/CodeGen/SIDSScheduler.h' file not found
```

**原因**：
- 头文件路径错误
- 或头文件未创建

**解决**：
```bash
# 检查头文件是否存在
ls llvm/include/llvm/CodeGen/SIDSScheduler.h

# 检查包含路径
grep "SIDSScheduler.h" llvm/lib/Target/WebAssembly/WebAssemblyTargetMachine.cpp
```

### 4.3 符号冲突

**错误信息**：
```
multiple definition of `llvm::createSIDSScheduler()'
```

**原因**：
- 工厂函数在头文件中定义（应该在 .cpp 中）
- 或多个文件包含了实现

**解决**：
- 确保 `createSIDSScheduler()` 只在 `SIDSScheduler.cpp` 中定义
- 头文件中只有声明

### 4.4 断言失败

**错误信息**：
```
Assertion `CurrentTop == CurrentBottom && "Nonempty unscheduled zone."' failed
```

**原因**：
- `pickNode()` 返回了 `nullptr`，但还有未调度的节点
- 通常是因为初始优先级设置不当

**解决**：
- 使用 `-std::numeric_limits<float>::infinity()` 作为初始优先级
- 见 [SIDS_MANUAL_PART2_IMPL.md](./SIDS_MANUAL_PART2_IMPL.md) 第 7.1 节

---

## 5. 调试技巧

### 5.1 使用 GDB 调试

```bash
# 编译 Debug 版本
cmake -DCMAKE_BUILD_TYPE=Debug ...
ninja llc

# 启动 GDB
gdb --args ./bin/llc -mtriple=wasm32-unknown-unknown test.ll -o test.s

# 设置断点
(gdb) break SIDSScheduler::pickNode
(gdb) run

# 查看变量
(gdb) print BestPriority
(gdb) print Best->NodeNum
```

### 5.2 使用 LLVM_DEBUG 输出

在代码中添加调试输出：

```cpp
LLVM_DEBUG(dbgs() << "Debug info: " << value << "\n");
```

编译并运行：

```bash
./bin/llc -debug -debug-only=sids-scheduler test.ll -o test.s 2>&1 | less
```

### 5.3 查看调度 DAG

```bash
./bin/llc -mtriple=wasm32-unknown-unknown \
  -enable-misched \
  -misched-print-dags \
  test.ll -o test.s 2> dag.log
```

**dag.log** 会包含：
- SUnit 列表
- 依赖关系
- 节点属性（Depth, Height 等）

### 5.4 对比不同调度器

```bash
# 生成默认调度的汇编
./bin/llc -mtriple=wasm32-unknown-unknown -misched=default test.ll -o default.s

# 生成 SIDS 调度的汇编
./bin/llc -mtriple=wasm32-unknown-unknown -misched=sids test.ll -o sids.s

# 对比
diff -u default.s sids.s | less
```

---

## 6. 性能分析

### 6.1 编译时间分析

```bash
# 测量编译时间
time ./bin/llc -mtriple=wasm32-unknown-unknown test.ll -o test.s
```

### 6.2 内存使用分析

```bash
# 使用 valgrind
valgrind --tool=massif ./bin/llc -mtriple=wasm32-unknown-unknown test.ll -o test.s

# 查看内存峰值
ms_print massif.out.* | less
```

### 6.3 性能剖析

```bash
# 使用 perf
perf record ./bin/llc -mtriple=wasm32-unknown-unknown large_test.ll -o test.s
perf report
```

---

## 7. 持续集成

### 7.1 自动化测试脚本

创建 `test_sids.sh`：

```bash
#!/bin/bash
set -e

BUILD_DIR="build-assert"
TEST_DIR="test_cases"

echo "=== Building LLVM with SIDS ==="
mkdir -p $BUILD_DIR
cd $BUILD_DIR
cmake -G Ninja ../llvm \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DLLVM_ENABLE_ASSERTIONS=ON \
  -DLLVM_TARGETS_TO_BUILD="WebAssembly"
ninja llc

echo "=== Running tests ==="
for test in ../$TEST_DIR/*.ll; do
  echo "Testing $test..."
  ./bin/llc -mtriple=wasm32-unknown-unknown \
    -enable-misched -misched=sids \
    $test -o /dev/null
done

echo "=== All tests passed ==="
```

### 7.2 Git 钩子

创建 `.git/hooks/pre-commit`：

```bash
#!/bin/bash
# 提交前自动测试

./test_sids.sh
if [ $? -ne 0 ]; then
  echo "Tests failed, commit aborted"
  exit 1
fi
```

---

## 8. 部署与分发

### 8.1 安装 LLVM

```bash
cd build-assert
ninja install
```

**安装位置**：
- 由 `CMAKE_INSTALL_PREFIX` 指定
- 默认：`/usr/local`

### 8.2 打包

```bash
# 创建 tarball
cd /path/to/install
tar czf llvm-sids.tar.gz bin/ lib/ include/

# 或使用 CPack
cd build-assert
cpack
```

---

## 9. 总结检查清单

### 构建配置
- [ ] 修改 `llvm/lib/CodeGen/CMakeLists.txt`
- [ ] 添加 `SIDSScheduler.cpp` 到源文件列表
- [ ] 运行 `cmake` 重新配置
- [ ] 编译成功（`ninja llc`）

### 功能验证
- [ ] 基础编译测试通过
- [ ] 调试输出正常（`-debug-only=sids-scheduler`）
- [ ] 参数调整生效（`-sids-alpha` 等）
- [ ] 回归测试通过（`ninja check-llvm-codegen`）

### 性能测试
- [ ] 运行性能基准测试
- [ ] 对比不同调度器的输出
- [ ] 验证性能提升

---

## 附录：完整构建脚本

```bash
#!/bin/bash
# 完整的 SIDS 调度器构建脚本

set -e

LLVM_SRC="/path/to/llvm-project"
BUILD_DIR="$LLVM_SRC/build-sids"

echo "=== Step 1: Configure CMake ==="
mkdir -p $BUILD_DIR
cd $BUILD_DIR
cmake -G Ninja $LLVM_SRC/llvm \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DLLVM_ENABLE_ASSERTIONS=ON \
  -DLLVM_TARGETS_TO_BUILD="WebAssembly" \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache

echo "=== Step 2: Build llc ==="
ninja llc

echo "=== Step 3: Verify ==="
./bin/llc --version

echo "=== Step 4: Run basic test ==="
cat > /tmp/test.ll << 'EOF'
target triple = "wasm32-unknown-unknown"
define i32 @test(i32 %a) { ret i32 %a }
EOF

./bin/llc -mtriple=wasm32-unknown-unknown \
  -enable-misched -misched=sids \
  -debug -debug-only=sids-scheduler \
  /tmp/test.ll -o /tmp/test.s 2> /tmp/sids.log

if grep -q "SIDS Scheduler initialized" /tmp/sids.log; then
  echo "=== SUCCESS: SIDS scheduler is working ==="
else
  echo "=== FAILED: SIDS scheduler not working ==="
  exit 1
fi
```

---

## 下一步

- [返回主手册](./SIDS_INTEGRATION_MANUAL.md)
- [查看性能测试报告](./PAPER_EXPERIMENT_REPORT.md)
- [阅读常见问题](./SIDS_FAQ.md)
