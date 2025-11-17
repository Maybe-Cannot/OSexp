# 扩展最大文件大小到 65803 块（二级间接块）

本说明从 `user/bigfile.c` 入手，梳理 open/write 调用链到内核，并聚焦我们为“支持二级间接块”所做的最小必要改动：常量与结构、`bmap` 映射与 `itrunc` 释放、`mkfs` 侧的 `iappend`。

---

## 调用链总览（只聚焦相关路径）

- (user) `user/bigfile.c: main()`
  - `open("big.file", O_CREATE|O_WRONLY)`
    - (lib) `open()` -> (trap) 系统调用
    - (kernel) `sys_open` in `kernel/sysfile.c`
      - 若包含 `O_CREATE`：调用 `create()` 分配 inode
      - 打开后返回文件描述符 `fd`
  - 循环 `write(fd, buf, BSIZE)` 直到设备/文件系统写不动
    - (lib) `write()` -> (trap)
    - (kernel) `sys_write` in `kernel/sysfile.c`
      - `filewrite(f, p, n)` in `kernel/file.c`
        - `writei(ip, ...)` in `kernel/fs.c`
          - 反复调用 `bmap(ip, bn)` 将“文件内逻辑块号 bn”映射到“磁盘块号”
          - 真正的数据写入由日志层/磁盘驱动完成
  - `close(fd)` -> `sys_close` -> `fileclose`

> 关键点：大文件能否写成功，取决于 `bmap()` 是否能为“越来越大的 bn”分配对应的数据块（需支持二级间接索引）。

---

## 为何是 65803 块？计算来源

- 设定：`BSIZE = 1024` 字节，指针大小 `sizeof(uint)=4` 字节
- 每个间接块能容纳 `NINDIRECT = BSIZE/sizeof(uint) = 256` 个块号
- 修改后的 inode 布局：
  - 直接块：`NDIRECT = 11`
  - 一级间接：`1` 个块，容量 `256`
  - 二级间接：`1` 个块，容量 `256*256`
- 因此最大数据块数：
  - `MAXFILE = NDIRECT + NINDIRECT + NINDIRECT*NINDIRECT`
  - `= 11 + 256 + 256*256 = 11 + 256 + 65536 = 65803`

这正对应 `user/bigfile.c` 中的校验：`if (blocks != 65803) ...`。

---

## 我们做了哪些修改（核心最小集）

### 1) 常量与结构（`kernel/fs.h`，以及与其一致的内存 inode 结构）

- 更新并约定：
  - `#define NDIRECT 11`
  - `#define NINDIRECT (BSIZE / sizeof(uint))  // 256`
  - `#define MAXFILE (NDIRECT + NINDIRECT + NINDIRECT * NINDIRECT)`
- 扩展磁盘 inode(`struct dinode`)与内存 inode(`struct inode`)的 `addrs[]`：
  - 原：`NDIRECT + 1`（直接 + 一级间接）
  - 新：`NDIRECT + 2`（直接 + 一级间接 + 二级间接）
  - 约定索引：
    - `addrs[0..NDIRECT-1]`：11 个直接块
    - `addrs[NDIRECT]`：一级间接块号
    - `addrs[NDIRECT+1]`：二级间接块号

> 注意：`file.h` 中内存 inode 的 `addrs` 声明需与 `fs.h` 保持一致。

### 2) 块映射 `bmap()`（`kernel/fs.c`）

- 原逻辑：只处理直接块与一级间接块；当 `bn >= NDIRECT + NINDIRECT` 时失败
- 新增：二级间接处理。

伪代码（仅展示核心分支）：

```c
if (bn < NDIRECT) {
  // 直接块：按需分配数据块
} else if (bn < NDIRECT + NINDIRECT) {
  // 一级间接：按需分配间接块和数据块
} else {
  // 二级间接
  uint bn2 = bn - (NDIRECT + NINDIRECT); // 二级区域内的偏移
  uint l1 = bn2 / NINDIRECT;             // 第几个一级间接块
  uint l0 = bn2 % NINDIRECT;             // 该一级间接块中的第几个数据块

  // 1) 分配/读取“二级间接块”（保存 NINDIRECT 个“一级间接块号”）
  // 2) 若 a1[l1] 为 0，则分配一个“一级间接块”，写回 a1
  // 3) 读取这个“一级间接块”，若 a0[l0] 为 0，则分配数据块，写回 a0
  // 4) 返回最终的数据块号
}
```

要点：
- 需要用 `bread/bwrite/brelse` 读写并持久化两层索引块的数组
- 分配使用 `balloc()`，并确保对脏块调用 `log_write()`

### 3) 截断释放 `itrunc()`（`kernel/fs.c`）

- 原逻辑：释放直接块与一个一级间接块
- 新增：完整释放二级间接块层次：
  1) 遍历并释放所有直接块
  2) 若一级间接存在：读入，释放其指向的所有数据块，再释放该间接块本身
  3) 若二级间接存在：
     - 读入“二级间接块”的数组 a1
     - 对每个非 0 的 a1[i]（即某个一级间接块）：
       - 读入该一级间接块的数组 a0
       - 释放其中所有数据块
       - 释放该一级间接块
     - 最后释放二级间接块本身
  4) 将 `ip->size = 0`，并持久化更新 inode

> 这样在 `unlink`、`O_TRUNC` 或删除大文件时不会泄漏任何数据块或间接索引块。

### 4) `mkfs` 的 `iappend()`（`mkfs/mkfs.c`）

- `mkfs` 负责在构建镜像时向文件追加数据块，它也有一份简化版的块映射逻辑
- 需要与内核的 `bmap()` 一致地处理：
  - 11 个直接
  - 1 个一级间接（256）
  - 1 个二级间接（256×256）
- 更新其 `MAXFILE`/边界断言，避免在制作包含大文件的镜像时出错

---

## 关键数据流：以一次 `write(fd, buf, BSIZE)` 为例

1) 用户态 `write()` -> 陷入内核 -> `sys_write`
2) `sys_write` -> `filewrite(f, p, n)`
3) `filewrite` -> `writei(ip, ..., n)`
4) `writei` 计算文件内逻辑块号 `bn`，并调用 `bmap(ip, bn)`：
   - 若 `bn < 11`：直接块下标 -> 直接映射
   - 若 `11 <= bn < 11 + 256`：一级间接
   - 若 `bn >= 11 + 256`：二级间接（先定位到某个“一级间接块”，再在其中定位“数据块”）
5) 得到磁盘块号后，走日志/缓冲层写入，最终落盘

---

## 边界与错误处理要点

- `bmap()` 对越界 `bn` 必须返回 0/错误，调用方据此停止写入
- `balloc()`/`bread()` 失败要谨慎处理（xv6 通常 `panic`）
- 对所有被修改的元数据块，调用 `log_write()` 确保持久化
- 释放路径要对“可能为 0 的块号”做健壮判断，避免重复释放

---

## 为什么 bigfile 现在能写满 65803 块

- 因为 `bmap()` 在直接+一级间接耗尽后，能正确：
  - 分配“二级间接块”
  - 在其中分配/引用“一级间接块”
  - 在“一级间接块”中分配数据块
- `itrunc()` 在清理时能递归式地释放上述所有层级资源
- `mkfs` 的 `iappend()` 与内核逻辑一致，保证构建镜像时也能处理大文件

---

## 涉及的文件一览

- `user/bigfile.c`：用户态测试程序（65803 块）
- `kernel/sysfile.c`：`sys_open`/`sys_write` 等系统调用入口
- `kernel/file.c`：`filewrite` 等面向文件对象的 I/O 封装
- `kernel/fs.c`：`bmap`、`writei`、`itrunc` 等核心文件系统逻辑（本次改动重点）
- `kernel/fs.h`：常量/结构定义（`NDIRECT`、`MAXFILE`、inode 布局）
- `kernel/file.h`：内存 inode 结构，需与 `fs.h` 对齐
- `mkfs/mkfs.c`：制作镜像时向文件追加数据（`iappend` 对齐内核的映射规则）

---

## 附：简图（inode 块指针布局）

```
             inode.addrs[0..10]  --> 11 个直接数据块
             inode.addrs[11]     --> 一级间接块 (256 指针)
             inode.addrs[12]     --> 二级间接块 (256 指针，每个指向一个一级间接块)
                                                     └─ 每个一级间接块再指向 256 个数据块
最大数据块数 = 11 + 256 + 256*256 = 65803
```

如需，我可以在上述各文件中插入对照注释（标出新增与修改的行）或补充代码段讲解具体实现。