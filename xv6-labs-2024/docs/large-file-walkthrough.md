# 扩展文件大小（65803 块）实现说明（从 bigfile 出发）

本文参考“超级页支持”文档的行文结构，从测试、思路到实现细节，系统梳理“二级间接块”以支持最大 65803 个数据块的实现。只关注本实验核心逻辑（块地址映射/释放/构建镜像对齐）。

- 绿色：无需修改的函数/路径
- 黄色：需要修改的已有函数
- 红色：新增或等价于“新职责”的实现

---

## 0. 测试脚本与目标（bigfile.c）

测试位于 `user/bigfile.c`：
- open("big.file", O_CREATE|O_WRONLY)，随后循环 write 直到写不动为止；统计写入的块数 blocks；
- 期望 blocks 恰好等于 65803；
- 关闭后再以 O_RDONLY 打开并顺序读取，校验每个块头写入的整数是否与块号一致。

核心目标：让文件系统支持“11 个直接块 + 1 个一级间接块(256) + 1 个二级间接块(256×256)”共 65803 个数据块。

---

## 1. 常量与结构（fs.h/file.h）  [黄色]

- 将 `NDIRECT` 调整为 11；
- 保持 `NINDIRECT = BSIZE/sizeof(uint) = 256`；定义 `N2INDIRECT = NINDIRECT*NINDIRECT`；
- 计算 `MAXFILE = NDIRECT + NINDIRECT + N2INDIRECT = 65803`；
- 扩充 `struct dinode` 与内存 `struct inode` 的 `addrs[]`：从 `NDIRECT+1` 变更为 `NDIRECT+2`；
  - `addrs[0..NDIRECT-1]`：直接块
  - `addrs[NDIRECT]`：一级间接块（指向“数据块地址数组”）
  - `addrs[NDIRECT+1]`：二级间接块（指向“一级间接块地址数组”）

元操作解释：
- 这些宏/结构决定“bmap/writei/itrunc”的边界判断与寻址方式；
- `addrs` 的新增槽位用于挂接“二级间接块”。

---

## 2. 调用链与职责划分（从 bigfile 出发）

- (user) bigfile main()
  - open(...) → (kernel) sys_open → create/namei → 返回 fd  [绿色]
  - write(fd, buf, BSIZE) → sys_write → filewrite → writei
    - writei 中按文件内偏移计算逻辑块号 bn，并调用 bmap(ip, bn) 获取/分配物理块号 [黄色]
      - bmap：负责直接/一级/二级间接的地址映射与“按需分配”（balloc） [黄色]
    - 取得块号后，走日志/缓冲层写盘 [绿色]
  - close(fd) → sys_close → fileclose [绿色]
  - 再次 open 只读 → read 校验 [绿色]

---

## 3. 映射与分配：bmap(ip, bn)  [黄色]

要求：当 bn 超出“直接 + 一级间接”范围后，进入“二级间接”路径；并在必要时分配对应层级的索引块与数据块。

概要逻辑：
- 直接块：`bn < NDIRECT` → `ip->addrs[bn]` 为空则 `balloc()` 分配数据块；
- 一级间接：`NDIRECT <= bn < NDIRECT+NINDIRECT` → 如无“一级间接块头”则 `balloc()` 之；装载块头数组，若槽位空则分配数据块并 `log_write()`；
- 二级间接：`bn >= NDIRECT+NINDIRECT` 且 `< NDIRECT+NINDIRECT+N2INDIRECT` → 如无“二级间接块”则分配；装载其数组得到某个“一级间接块地址”，如空则分配并 `log_write()`；再装载该“一级间接块”，若对应槽位数据块为空则分配并 `log_write()`。

元操作解释：
- balloc(dev)：从位图找空闲块（置位）+ 对被分配块清零（bzero） → “分配空间”；
- bread(dev, bno)/brelse(bp)：把某块装入/释放缓存，便于读/改“指针数组”；
- log_write(bp)：把修改过的“元数据块”（间接/二级间接）纳入事务，保证崩溃后可恢复；
- 返回的数据块号交给 writei 后续写入。

边界保护：
- writei 在写入前检查 `off + n <= MAXFILE*BSIZE`，防止越界；
- bmap 超界时 `panic("bmap: out of range")` 或返回 0（磁盘耗尽）以停止上层写入。

---

## 4. 释放：itrunc(ip)  [黄色]

要求：在 `unlink`/`O_TRUNC`/回收时，彻底释放“直接块、一级间接块、二级间接块及其子块”。

释放顺序：
1) 遍历直接块，若非 0 则 `bfree()`，并清零条目；
2) 若存在一级间接块：装载其数组，释放所有数据块 → 释放“一级间接块头本身” → 清零；
3) 若存在二级间接块：
   - 读入“二级间接块”的数组，逐个取出“一级间接块地址”；
   - 对每个“一级间接块”，装载其数组并释放所有数据块 → 释放“一级间接块本身”；
   - 最后释放“二级间接块本身”并清零；
4) `ip->size = 0` 且 `iupdate(ip)` 落盘。

元操作解释：
- bfree(dev, bno)：把位图对应位清 0 → “归还空间”；
- 必须先释放子块，再释放承载指针的父块，避免悬挂引用。

---

## 5. 构建镜像一致性：mkfs/mkfs.c::iappend()  [黄色]

要求：镜像构建阶段的“追加数据”规则与内核 bmap 一致，才能在大文件上工作。

逻辑：
- fbn = off/BSIZE；断言 `fbn < MAXFILE`；
- 直接块 → 若空则分配数据块（`freeblock++`）并写数据；
- 一级间接 → 若“间接块头”空则分配；装载数组并为空槽分配数据块；
- 二级间接 → 若“二级间接块”空则分配；装载其数组、为目标“一级间接块地址”分配；装载该一级间接块并为数据槽分配数据块；
- 使用 `rsect/wsect` 直接读/写镜像扇区，不走日志；
- 最终更新 dinode.size 并写回。

元操作解释：
- `freeblock++` 是 mkfs 阶段的“分配计数器”，等价于运行时 `balloc()` 的“分配空间”语义；
- rsect/wsect：直接 I/O 镜像文件的扇区；xint/xshort：主机字节序 → 镜像字节序。

---

## 6. 验证步骤  [绿色]

- 编译、启动内核（无源码变更的路径略）；
- 在 shell 中运行 `bigfile`：
  - 期望输出类似：`wrote 65803 blocks`、`reading bigfile`、`bigfile done; ok`；
- 如果 blocks 小于 65803：
  - 优先检查：`NDIRECT` 是否为 11；`MAXFILE` 是否匹配；bmap 是否包含“二级间接”分支；writei 越界判断是否使用新 `MAXFILE`；mkfs 是否同步修改。

---

## 7. 与“超级页支持”的对照

- 两者共同之处：
  - 都是“扩大容量/粒度”的系统支持，需要在分配/映射/回收三条链路上保持一致；
- 本实验差异：
  - 关注的是“文件块寻址层级”的扩展（指针索引树），而非页表层级；
  - 关键原语是 `balloc/bfree/bread/log_write/brelse` 与 `iupdate`，而非页表 `walk/mappages/uvmunmap`。

---

## 8. 变更清单速览

- fs.h：`NDIRECT=11`，定义 `N2INDIRECT`，`MAXFILE` 更新，`struct dinode.addrs[NDIRECT+2]`；
- file.h：`struct inode.addrs[NDIRECT+2]` 同步；
- fs.c：`bmap()` 增加“二级间接”分支；`itrunc()` 增加“二级间接释放”；`writei()` 使用新 `MAXFILE*BSIZE`；
- mkfs.c：`iappend()` 覆盖“二级间接”路径，增加 `assert(fbn < MAXFILE)`。

---

## 9. 常见问题（FAQ）

- Q：为什么把直接块从 12 改为 11？
  - A：为在固定大小的 `addrs[]` 中留出一个槽位存放“二级间接块”指针；
- Q：为什么要在修改指针数组块后调用 `log_write()`？
  - A：这些块是“元数据块”，需纳入日志以保证崩溃恢复的一致性；
- Q：mkfs 一定要改吗？
  - A：当需要在镜像构建阶段写入大文件时必须改，否则会在 12+256 上限处失败。

---

如需配套的“原代码/修改后代码/修改目的 + 元操作解释”的逐处对照，请参见 `docs/large-file-diff.md`；本文更侧重从测试到实现流程的叙事式说明。