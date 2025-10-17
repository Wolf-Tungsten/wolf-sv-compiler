# corvus-sv-compiler
支持SystemVerilog的自动RTL划片编译器

## 项目简介

Corvus-SV-Compiler 是一个基于 [slang](https://github.com/MikePopoloski/slang) 的 SystemVerilog 编译器，用于分析和处理 SystemVerilog RTL 设计。

## 构建项目

### 前置要求

- CMake 3.20 或更高版本
- C++20 兼容的编译器（GCC 11+, Clang 14+, MSVC 2019+）
- Git

### 构建步骤

```bash
# 克隆仓库（包含子模块）
git clone --recursive https://github.com/Wolf-Tungsten/corvus-sv-compiler.git
cd corvus-sv-compiler

# 如果已经克隆但没有子模块，运行：
git submodule update --init --recursive

# 构建项目
mkdir build
cd build
cmake ..
cmake --build . -j$(nproc)
```

构建完成后，可执行文件将生成在 `build/corvus-sv-compiler`。

## 使用方法

### 基本用法

```bash
# 编译单个 SystemVerilog 文件
./corvus-sv-compiler <file.sv>

# 编译多个文件
./corvus-sv-compiler file1.sv file2.sv

# 指定顶层模块
./corvus-sv-compiler --top <module_name> <file.sv>
```

### 支持的选项

corvus-sv-compiler 支持 slang 的所有标准命令行选项，包括：

- `--top <name>` - 指定顶层模块
- `--define <macro>=<value>` - 定义预处理宏
- `-I <path>` - 添加包含路径
- `-L <path>` - 添加库路径
- 等等...

更多选项请参考 [slang 文档](https://sv-lang.com/user-manual.html)。

## 项目结构

```
corvus-sv-compiler/
├── CMakeLists.txt          # CMake 构建配置
├── src/                    # 源代码目录
│   └── main.cpp           # 主程序入口
├── include/               # 头文件目录
├── external/              # 外部依赖
│   └── slang/            # slang 子模块
└── build/                # 构建目录（生成）
```

## 许可证

本项目使用与 slang 相同的 MIT 许可证。

