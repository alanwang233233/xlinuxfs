# xlinuxfs App Store Listing Draft

GitHub: https://github.com/HuanchuanTech/xlinuxfs

This document keeps the App Store copy plain and conservative. It highlights the real technical choices: FSKit instead of macFUSE, the real Linux kernel (via LKL) as the filesystem engine, automatic mounting through macOS, a simple control-panel UI, diagnostics, and guided command-line steps for the actions the sandbox can't perform directly.

> Distribution note: xlinuxfs links the Linux kernel and its ext4 / XFS / Btrfs drivers, which are GPL-2.0. Distributing GPL-licensed software through the App Store has well-known constraints — confirm the licensing and distribution path before submitting. This draft is the listing copy only.

## English

### App Name

xlinuxfs

### Subtitle

Read/write ext4, XFS & Btrfs

### Promotional Text

Read and write ext4, XFS, and Btrfs drives on macOS using a modern FSKit file-system extension, powered by the real Linux kernel via LKL — no kernel extension, no macFUSE.

### Description

xlinuxfs is a simple Linux-filesystem utility for macOS. It uses Apple's FSKit file-system extension model and the real Linux kernel — run in-process with LKL (Linux Kernel Library) — to work with ext2/3/4, XFS, and Btrfs volumes, without a kernel extension and without macFUSE.

Because the on-disk handling is done by the actual Linux kernel drivers, journaling and recovery behave the way they do on Linux. Once the file-system extension is installed and enabled, macOS can automatically mount supported Linux devices under /Volumes. The app itself stays lightweight: it shows your Linux drives and disk images, lets you mount or eject volumes, and gives clear status information when something needs attention.

What xlinuxfs focuses on:

- FSKit-based Linux-filesystem support, not a macFUSE layer
- The real Linux kernel (via LKL) as the underlying ext / XFS / Btrfs engine
- Read and write ext2/3/4, XFS, and Btrfs — the actual in-kernel Linux drivers
- Automatic mounting for Linux devices through macOS Disk Arbitration
- A small, practical interface for drives, disk images, mount status, and read-only choices
- Per-scenario read-only defaults (disk drives vs disk images), shared with the extension
- Guided command-line steps for the mounting and disk-image actions the sandbox can't perform directly
- Diagnostics for extension installation, enablement, duplicate registrations, and common setup issues
- Copyable Terminal commands when the sandbox prevents the app from safely doing something directly
- Open-source development, with source code available on GitHub

xlinuxfs is intentionally modest. It is not a file manager, backup tool, disk repair suite, or commercial storage platform. It is a focused bridge between macOS FSKit and the Linux kernel's filesystems, designed to make Linux volumes easier to use on a Mac while keeping the behavior visible and understandable.

Source code:
https://github.com/HuanchuanTech/xlinuxfs

Note: ext4, XFS, and Btrfs are Linux filesystem formats. xlinuxfs is an independent open-source project and is not affiliated with or endorsed by the Linux Foundation, the Linux kernel developers, Apple, or the LKL maintainers.

### Keywords

ext4,ext3,XFS,Btrfs,Linux,FSKit,LKL,macFUSE,disk,drive,USB,read,write,mount

### What's New

Initial release with FSKit-based mounting of ext2/3/4, XFS, and Btrfs via an in-process Linux kernel (LKL), automatic device detection, disk image workflows, and extension diagnostics.

### Review Notes

xlinuxfs includes an FSKit file-system extension. The extension must be enabled by the user in System Settings before macOS can mount Linux volumes through it. Some operations are shown as copyable Terminal commands because the sandbox does not allow the app to run privileged mounting tools directly.

## 简体中文

### 应用名称

xlinuxfs

### 副标题

读写 ext4、XFS、Btrfs

### 宣传语

用 macOS 的 FSKit 文件系统扩展以读写方式挂载 ext4、XFS、Btrfs 磁盘,底层使用通过 LKL 在进程内运行的真正 Linux 内核,不需要内核扩展,也不依赖 macFUSE。

### 应用介绍

xlinuxfs 是一个简单、直接的 macOS Linux 文件系统工具。它使用 Apple 的 FSKit 文件系统扩展模型,并通过 LKL(Linux Kernel Library)在进程内运行真正的 Linux 内核,来处理 ext2/3/4、XFS、Btrfs 卷,不需要内核扩展,也不依赖 macFUSE。

由于对磁盘格式的处理交给真正的 Linux 内核驱动,日志与恢复的表现和在 Linux 上一致。安装并启用文件系统扩展后,macOS 可以把支持的 Linux 设备自动挂载到 /Volumes。xlinuxfs 应用本身更像一个轻量控制面板:显示当前的 Linux 磁盘和磁盘映像,提供挂载、推出、只读选择和状态查看,并在出现问题时给出清楚的诊断信息。

xlinuxfs 关注这些事情:

- 使用 FSKit 文件系统扩展,而不是 macFUSE 层
- 底层使用真正的 Linux 内核(通过 LKL)处理 ext / XFS / Btrfs
- ext2/3/4、XFS、Btrfs 读写——由真正的 Linux 内核驱动处理
- 通过 macOS Disk Arbitration 自动挂载 Linux 设备
- 界面简单,主要用于查看磁盘、磁盘映像、挂载状态和只读选项
- 按场景的只读默认值(磁盘驱动器与磁盘映像),并与扩展共享
- 为沙盒无法直接执行的挂载与附加映像操作提供引导式命令行步骤
- 诊断扩展是否安装、是否启用、是否存在重复注册或常见配置问题
- 当沙盒不允许应用直接执行某些操作时,提供可复制的终端命令
- 开源开发,源代码可在 GitHub 查看

xlinuxfs 有意保持质朴。它不是文件管理器、备份工具、磁盘修复套件,也不是完整的商业存储平台。它专注于把 macOS FSKit 和 Linux 内核的文件系统连接起来,让 Linux 卷在 Mac 上更容易使用,同时尽量把行为解释清楚。

源代码:
https://github.com/HuanchuanTech/xlinuxfs

说明:ext4、XFS、Btrfs 是 Linux 文件系统格式。xlinuxfs 是独立开源项目,与 Linux 基金会、Linux 内核开发者、Apple 或 LKL 维护者没有从属或背书关系。

### 关键词

ext4,XFS,Btrfs,Linux,FSKit,LKL,macFUSE,磁盘,U盘,读写,挂载

### 更新说明

首个版本,提供基于 FSKit、经由进程内 Linux 内核(LKL)挂载 ext2/3/4、XFS、Btrfs,自动设备检测、磁盘映像流程和扩展诊断。

### 审核备注

xlinuxfs 包含一个 FSKit 文件系统扩展。用户需要先在系统设置中启用该扩展,macOS 才能通过它挂载 Linux 卷。部分操作会显示为可复制的终端命令,因为沙盒不允许应用直接运行需要额外权限的挂载工具。

## SEO Notes

Use these only where appropriate, such as a project website, support page, or release notes. For App Store metadata, third-party trademarks can be sensitive, so do not imply affiliation, compatibility certification, endorsement, or replacement guarantees.

### English SEO Phrases

- ext4 for macOS without macFUSE
- FSKit Linux filesystem driver for macOS
- mount ext4 / XFS / Btrfs on macOS
- read Linux drives on a Mac
- open-source Linux-filesystem utility for Mac
- alternative search terms: Paragon extFS, macFUSE ext4, ext4 on Mac

### 中文 SEO 短语

- macOS Linux 文件系统工具
- 不依赖 macFUSE 的 ext4 方案
- 基于 FSKit 的 Linux 文件系统扩展
- 在 Mac 上挂载 ext4 / XFS / Btrfs
- 开源 Mac Linux 磁盘工具
- 相关搜索词:Paragon extFS、macFUSE ext4、ext4 Mac
