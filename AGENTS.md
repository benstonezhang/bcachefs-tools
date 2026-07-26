# 项目概述

本项目是 `bcachefs-tools` 项目的一个特殊版本，旨在为 bcachefs 文件系统提供完整的、纯 C 语言编写的用户空间工具。 该版本特别适用于没有 Rust 工具链的环境，或者对二进制文件体积有严格要求的场景。

项目包含了核心的 `bcachefs` 命令行工具，以及标准的系统包装脚本，如 `mkfs.bcachefs`、`fsck.bcachefs` 和 `mount.bcachefs`。

## 主要技术栈

- **C11**: 核心库和工具的主要开发语言。
- **GNU Make**: 基础构建系统。
- **Nix (Flakes)**: 提供声明式的构建和开发环境。
- **Rust**: 虽然项目目前处于向纯 C 迁移的过程中，但 `src/` 目录下仍保留了同步更新的 Rust 实现作为参考。

## 目录结构

- **`fs/`**: 核心库，与 Linux 内核中的 bcachefs 实现保持高度同步。此目录下的任何文件都不要修改。
- **`c_src/`**: 主要工具及其子命令（如 `format`、`mount`、`fs`、`subvolume`）的 C 语言实现（逻辑与 Rust 版本保持严格一致）。文件名中包含shims的代码和libbcachefs.c为rust粘合代码，不参加C代码编译。
- **`src/`**: 工具的 Rust 语言实现。此目录下的任何文件都不要修改。
- **`linux/`, `ccan/`, `raid/`: 支撑库、内核风格的工具函数以及兼容性头文件。这些目录下的任何文件都不要修改。
- **`bch_bindgen/`**: 用于生成 FFI 绑定的内部工具，作为代码转换的参考。
- **`bcachefs-shim`, `bcachefs-shim-macros`**: rust用户空间内核兼容层

# 构建与运行

### 常用构建命令

- **标准构建**: `make`
- **启用FUSE 支持**: `BCACHEFS_FUSE=1 make`（需要安装 fuse3 3.7或以上版本）
- **启用增强型 TUI (timestats)**: `BCACHEFS_NCURSES=1 make` (需要安装 ncurses 库)
- **安装**: `make install`

### Nix 常用命令

- **使用 Nix 构建**: `nix build`
- **进入开发环境**: `nix develop` (提供 GCC, `clang-tools`, `gdb`, `valgrind` 以及所有依赖库)

# 开发规范

### 代码组织

- **Commands** src/commands目录下的rust代码对应的是src/commands目录下的C代码；一个例外是src/commands/opts.rs，它对应的C代码在src/opts.c，因为它本身不提供子命令。
- **辅助文件** src目录下的其他rust代码对应的是src目录下C文件，而相应的函数声明大部分都在libbcachefs.h中

### 编码风格

- **内核风格 C**: 代码遵循 Linux 内核命名规范和数据类型（如 `u64`, `s64`）。
- **错误处理**: 致命错误使用 `die()`，带检查的系统调用使用 `xioctl()`/`xopen()` 等。
- **缓冲区管理**: 所有格式化输出应使用 `printbuf` 结构，以确保列对齐和缩进的一致性。
- **表格式输出**: 表格式输出主要利用 `printbuf_tabstop_push/printbuf_tabstop_pop/printbuf_tabstops_reset` 定制制表符位置来实现。需要注意的是 `prt_printf` 对 `'\r'` 的特殊处理必须在已设置自定义制表位的情况下才能工作。
- **注释**: 在转换 rust 代码时，将注释一并同步到 C 代码。同时尽量保持 C 代码的函数顺序和布局与 rust 代码一致。
- **修改** 在进行代码对照检查或错误修改时，总是重新读需要修改的文件，以免破坏外部修改内容。在输出修改的代码时，需要同时给出修改的原因。

### 核心逻辑一致性 (Parity)

- **核心库不可改动**
- **Rust 代码不可改动**
- **C 与 Rust 对等**: 项目的首要开发目标是保持 `c_src/` 与 `src/` 之间的严格逻辑对等。Rust 侧核心逻辑的改动必须同步到 C 一侧。同时也尽量保证代码文件命名的一一对应。
- **参数解析**: 复杂的命令（如 `format` 和 `image`）使用手写的参数解析循环而非标准库，以处理位置敏感的、针对特定设备的选项。
- **剔除check_kernel_warnings**: 完全不需要
- **剔除http服务**: 不需要http服务带来的额外复杂性
- **不要改动的C文件**: c_src目录下文件名中包含shims的，以及libbcachefs.c不参加C代码的编译

### 版本管理

- 版本号从 `Changelog.mdwn` 提取（通常是第一行以 `## vX.Y.Z` 开头的内容）。
- `version.h` 头文件在构建过程中动态生成。
