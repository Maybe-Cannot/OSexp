# 二级间接块扩展修改详解

本文件列出为支持最大 65803 数据块（11 直接 + 1 一级间接 + 1 二级间接）所做的每一处源代码修改：
- 原代码（来自标准 xv6 / 本实验修改前常见版本）
- 修改后代码（当前仓库）
- 修改目的与说明

> 说明："原代码" 片段基于 xv6-labs-2024 初始结构的传统实现（12 个直接块 + 1 个一级间接，不支持二级间接）。若你的本地初始版本略有差异，可按思路比对。

---
## 1. `kernel/fs.h` 常量与磁盘 inode 结构

### 修改点 1：调整直接块数量，增加二级间接支持并重算最大文件大小
**原代码：**
```c
#define NDIRECT 12
#define NINDIRECT (BSIZE / sizeof(uint))
#define MAXFILE (NDIRECT + NINDIRECT)
struct dinode {
  short type;
  short major;
  short minor;
  short nlink;
  uint size;
  uint addrs[NDIRECT+1]; // 12 direct + 1 indirect
};
```
**修改后代码：**
```c
// 直接块数量调整为 11，预留出一个位置给一级间接和一个位置给二级间接块。
#define NDIRECT 11
#define NINDIRECT (BSIZE / sizeof(uint))
#define N2INDIRECT (NINDIRECT * NINDIRECT)
#define MAXFILE (NDIRECT + NINDIRECT + N2INDIRECT)
struct dinode {
  short type;
  short major;
  short minor;
  short nlink;
  uint size;
  // [0..NDIRECT-1] 直接块
  // [NDIRECT] 一级间接块
  // [NDIRECT+1] 二级间接块
  uint addrs[NDIRECT+2];
};
```
**目的：**
- 将 12 个直接块压缩为 11 个，腾出一个条目位置用于存放二级间接块指针。
- 新增二级间接块能力：一个二级间接块包含 256 个一级间接块地址，每个一级间接块再含 256 个数据块地址，共 65536。
- 更新 `MAXFILE` 公式以匹配新容量：`11 + 256 + 256*256 = 65803`。

### 修改点 2：新增解释性中文注释
**目的：** 便于理解层次结构与扩展策略。

#### 元操作解释（这个文件的“操作”是概念层面的）
- NDIRECT、NINDIRECT、N2INDIRECT、MAXFILE：这些宏用于界定“映射范围”与“越界判断”的上限，供 `bmap()` 和 `writei()` 使用。
- dinode.addrs[] 长度改为 NDIRECT+2：多出的 1 个槽位用于“二级间接块”指针。
- addrs 的三段意义：
  1) 直接块区：addrs[0..NDIRECT-1]
  2) 一级间接：addrs[NDIRECT]
  3) 二级间接：addrs[NDIRECT+1]

---
## 2. `kernel/file.h` 内存 inode 结构同步

**原代码：**
```c
struct inode {
  ...
  uint size;
  uint addrs[NDIRECT+1]; // 与旧磁盘 inode 对齐
};
```
**修改后代码：**
```c
struct inode {
  ...
  uint size;
  // 与磁盘 inode 对齐：增加二级间接。
  uint addrs[NDIRECT+2];
};
```
**目的：** 保持内存结构与磁盘结构一致，确保读写/复制 (`memmove`) 时不会溢出或遗漏二级间接指针。

#### 元操作解释
- 该结构与 `struct dinode` 保持字段布局上的“逻辑一致性”。
- `ilock()` 会把磁盘上的 dinode 读入到内存 inode（包括 addrs[]）——因此数组长度必须同步。
- `iupdate()` 会把内存 inode 的 addrs[] 拷贝回磁盘——长度也必须同步。

---
## 3. `kernel/fs.c` 块映射函数 `bmap()`

### 原代码（精简展示）：
```c
static uint bmap(struct inode *ip, uint bn) {
  uint addr, *a; struct buf *bp;
  if(bn < NDIRECT){
    if((addr = ip->addrs[bn]) == 0)
      ip->addrs[bn] = addr = balloc(ip->dev);
    return addr;
  }
  bn -= NDIRECT;
  if(bn < NINDIRECT){
    if((addr = ip->addrs[NDIRECT]) == 0)
      ip->addrs[NDIRECT] = addr = balloc(ip->dev);
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    if((addr = a[bn]) == 0){
      a[bn] = addr = balloc(ip->dev);
      log_write(bp);
    }
    brelse(bp);
    return addr;
  }
  panic("bmap: out of range");
}
```

### 修改后代码（包含二级间接处理）：
```c
static uint bmap(struct inode *ip, uint bn) {
  uint addr, *a; struct buf *bp;
  if(bn < NDIRECT){ ... } // 直接块
  bn -= NDIRECT;
  if(bn < NINDIRECT){ ... } // 一级间接
  bn -= NINDIRECT;
  if(bn < N2INDIRECT){
    if((addr = ip->addrs[NDIRECT+1]) == 0){ // 分配二级间接块
      addr = balloc(ip->dev);
      if(addr == 0) return 0;
      ip->addrs[NDIRECT+1] = addr;
    }
    bp = bread(ip->dev, addr); // 读二级间接块
    a = (uint*)bp->data;
    uint level1_index = bn / NINDIRECT;
    uint level1_offset = bn % NINDIRECT;
    uint level1_addr = a[level1_index];
    if(level1_addr == 0){ // 分配一级间接块
      level1_addr = balloc(ip->dev);
      if(level1_addr == 0){ brelse(bp); return 0; }
      a[level1_index] = level1_addr;
      log_write(bp);
    }
    brelse(bp);
    bp = bread(ip->dev, level1_addr); // 读一级间接块
    a = (uint*)bp->data;
    addr = a[level1_offset];
    if(addr == 0){ // 分配数据块
      addr = balloc(ip->dev);
      if(addr){ a[level1_offset] = addr; log_write(bp); }
    }
    brelse(bp);
    return addr;
  }
  panic("bmap: out of range");
}
```
**目的：**
- 扩展逻辑，使 `bn` 超过一级间接范围后仍可继续映射。
- 分层：二级间接块 -> 一级间接块 -> 数据块。
- 按需分配各层并用 `log_write()` 记录元数据更新确保事务持久性。
- 出现磁盘耗尽或异常分配失败时，通过 `return 0` 让上层写入逻辑停止。

#### 元操作解释（逐步）
以下解释对应上面的三个分支（直接 / 一级间接 / 二级间接）：

- 直接块分支：
  1) 范围判断：`if (bn < NDIRECT)`
  2) 条件分配：`if (ip->addrs[bn] == 0) ip->addrs[bn] = balloc(ip->dev);`
    - balloc(ip->dev)：从位图中找到一个空闲磁盘块，标记为已用，并对该块进行清零（bzero）；用于“数据块”存储。
  3) 返回数据块号：交给上层 `writei()` 去 `bread()` 并写入数据。

- 一级间接分支：
  1) 范围换算：`bn -= NDIRECT; if (bn < NINDIRECT)`
  2) 条件分配“间接块头”：`if (ip->addrs[NDIRECT] == 0) ip->addrs[NDIRECT] = balloc(ip->dev);`
    - 该块不是数据块，而是“指针数组块”，里面存放 NINDIRECT 个“数据块地址”。
  3) 读取指针数组：`bp = bread(ip->dev, ip->addrs[NDIRECT]); a = (uint*)bp->data;`
    - bread：把“间接块头”读入缓存，`a[bn]` 即第 bn 个数据块地址槽位。
  4) 条件分配数据块：`if (a[bn] == 0) { a[bn] = balloc(...); log_write(bp); }`
    - 分配的是“数据块”。log_write(bp) 确保“间接块头”中新增的地址被日志记录并落盘。
  5) 释放缓存：`brelse(bp)`。
  6) 返回数据块号。

- 二级间接分支：
  1) 范围换算：`bn -= NINDIRECT; if (bn < N2INDIRECT)`
  2) 条件分配“二级间接块”：`if (ip->addrs[NDIRECT+1] == 0) ip->addrs[NDIRECT+1] = balloc(ip->dev);`
    - 该块存的是 NINDIRECT 个“一级间接块地址”。
  3) 读取二级间接块：`bp = bread(ip->dev, ip->addrs[NDIRECT+1]); a = (uint*)bp->data;`
    - 计算两级下标：`level1_index = bn / NINDIRECT`，`level1_offset = bn % NINDIRECT`。
  4) 条件分配“一级间接块”：`if (a[level1_index] == 0) { a[level1_index] = balloc(...); log_write(bp); }`
    - 这是一个新的“指针数组块”，其中保存 NINDIRECT 个“数据块地址”。
  5) 释放并读取该“一级间接块”：`brelse(bp); bp = bread(ip->dev, a[level1_index]); a = (uint*)bp->data;`
  6) 条件分配“数据块”：`if (a[level1_offset] == 0) { a[level1_offset] = balloc(...); log_write(bp); }`
  7) 释放缓存并返回数据块号：`brelse(bp); return addr;`

小结：
- balloc：真正“分配空间”的原语（位图置位 + 清零）。
- bread/brelse：把某个块装入/释放缓冲区，便于读写数组指针。
- log_write：把修改过的“元数据块”（间接块/二级间接块）纳入事务日志，确保崩溃恢复。

---
## 4. `kernel/fs.c` 截断函数 `itrunc()`

### 原代码（精简展示）：
```c
void itrunc(struct inode *ip){
  int i; struct buf *bp; uint *a;
  for(i=0; i<NDIRECT; i++) if(ip->addrs[i]){ bfree(...); ip->addrs[i]=0; }
  if(ip->addrs[NDIRECT]){
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a = (uint*)bp->data;
    for(i=0;i<NINDIRECT;i++) if(a[i]) bfree(ip->dev,a[i]);
    brelse(bp); bfree(ip->dev, ip->addrs[NDIRECT]); ip->addrs[NDIRECT]=0;
  }
  ip->size = 0; iupdate(ip);
}
```

### 修改后代码（新增二级间接释放）：
```c
void itrunc(struct inode *ip){
  int i,j,k; struct buf *bp; uint *a;
  // 释放直接块 ...
  // 释放一级间接块 ...
  if(ip->addrs[NDIRECT+1]){ // 二级间接
    bp = bread(ip->dev, ip->addrs[NDIRECT+1]);
    a = (uint*)bp->data; // a[j] 是一级间接块地址
    for(j=0;j<NINDIRECT;j++) if(a[j]){
      struct buf *bp1 = bread(ip->dev, a[j]);
      uint *a1 = (uint*)bp1->data;
      for(k=0;k<NINDIRECT;k++) if(a1[k]) bfree(ip->dev,a1[k]);
      brelse(bp1);
      bfree(ip->dev, a[j]); // 释放一级间接块
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT+1]);
    ip->addrs[NDIRECT+1] = 0;
  }
  ip->size = 0; iupdate(ip);
}
```
**目的：**
- 保证删除 / 截断大文件时彻底释放所有层级分配的索引块与数据块，防止“孤儿块”泄漏。
- 遍历方式：外层循环每个一级间接块，内层循环其数据块。

#### 元操作解释（逐步）
- 释放直接块：
  1) 遍历 `ip->addrs[0..NDIRECT-1]`，对非 0 的条目调用 `bfree(ip->dev, blkno)` 释放数据块；将条目清零。
- 释放一级间接：
  1) 若 `ip->addrs[NDIRECT] != 0`，先 `bread()` 读入该“间接块头”。
  2) 遍历其中的每个数据块地址 `a[j]`，对非 0 的调用 `bfree()` 释放。
  3) `brelse()` 释放缓冲；再对“间接块头本身”调用 `bfree()`；将 `ip->addrs[NDIRECT]=0`。
- 释放二级间接：
  1) 若 `ip->addrs[NDIRECT+1] != 0`，`bread()` 读入“二级间接块”。
  2) 遍历其中的每个“一级间接块地址” `a[j]`：
     - 若非 0，先 `bread()` 读入这个“一级间接块”，遍历释放其每个数据块地址 `a1[k]`（`bfree`），`brelse()`，再 `bfree()` 释放该“一级间接块”。
  3) `brelse()` 释放“二级间接块”的缓冲，`bfree()` 释放“二级间接块本身”，并清零 `ip->addrs[NDIRECT+1]`。
- 重置大小并落盘：`ip->size = 0; iupdate(ip);`

小结：
- bfree：真正“归还空间”的原语（位图清位）。
- 一定要先释放指向的子块，再释放承载指针的块本身，避免悬挂引用。

---
## 5. `mkfs/mkfs.c` 中 `iappend()` 文件构造逻辑

### 原代码（精简展示）：
```c
while(n > 0){
  fbn = off / BSIZE;
  if(fbn < NDIRECT){ ... }
  else if(fbn < NDIRECT + NINDIRECT){ ... } // 一级间接
  else panic("mkfs: file too big");
  // 写入数据 ...
}
```

### 修改后代码（新增二级间接处理）：
```c
while(n > 0){
  fbn = off / BSIZE;
  assert(fbn < MAXFILE);
  if(fbn < NDIRECT){ ... } // 直接块
  else if(fbn < NDIRECT + NINDIRECT){ ... } // 一级间接
  else { // 二级间接
    uint fbn2 = fbn - (NDIRECT + NINDIRECT);
    if(xint(din.addrs[NDIRECT+1]) == 0) din.addrs[NDIRECT+1] = xint(freeblock++);
    rsect(xint(din.addrs[NDIRECT+1]), (char*)doubly);
    uint level1_index = fbn2 / NINDIRECT, level1_offset = fbn2 % NINDIRECT;
    if(doubly[level1_index] == 0){ doubly[level1_index] = xint(freeblock++); wsect(...); }
    rsect(xint(doubly[level1_index]), (char*)indirect1);
    if(indirect1[level1_offset] == 0){ indirect1[level1_offset] = xint(freeblock++); wsect(...); }
    x = xint(indirect1[level1_offset]);
  }
  // 写入数据块 ...
}
```
**目的：**
- 使镜像创建工具能生成超过旧上限的大文件，保证与内核运行期块映射方式一致。
- 加入断言 `assert(fbn < MAXFILE)` 防止构建阶段越界。

#### 元操作解释（逐步，对应三种分支）
- 通用准备：
  1) `rinode(inum, &din)`：把镜像上的 dinode 读取到内存结构 `din`。
  2) `fbn = off / BSIZE`：定位本次要写的“文件块号”。
  3) `assert(fbn < MAXFILE)`：防止越界。

- 直接块：
  1) 若 `din.addrs[fbn]==0`，分配新的数据块号：`din.addrs[fbn] = xint(freeblock++)`。
  2) `x = xint(din.addrs[fbn])` 作为物理块号，用于实际写入。

- 一级间接：
  1) 若 `din.addrs[NDIRECT]==0`，分配一个“间接块头”（保存数据块地址数组）。
  2) `rsect(din.addrs[NDIRECT], indirect)` 读出数组，若槽位为空则填入 `freeblock++` 并 `wsect()` 写回。

- 二级间接：
  1) 若 `din.addrs[NDIRECT+1]==0`，分配“二级间接块”（保存一级间接块地址数组）。
  2) 读出该数组，定位 `level1_index`/`level1_offset`。
  3) 若对应的“一级间接块地址”为 0，分配之并写回二级数组。
  4) 读出该“一级间接块”，若对应数据块槽位为 0，分配之并写回。

- 数据写入：
  1) 读出目标数据块：`rsect(x, buf)`；
  2) 把输入数据拷贝到正确偏移：`bcopy(p, buf + off - fbn*BSIZE, n1)`；
  3) `wsect(x, buf)` 写回数据块；
  4) 更新 `off` 和 `din.size`，最终 `winode(inum, &din)` 持久化 dinode。

小结：
- mkfs 的 `freeblock++` 是“构建镜像时”的分配计数器；与运行时的 `balloc()` 等价于“分配空间”的元操作。
- 与内核不同，mkfs 直接读/写镜像文件的扇区（`rsect/wsect`），不走日志。

---
## 6. 相关次要改动与注释
- 在各新增逻辑附近加入中文注释解释层次与索引计算（`level1_index`, `level1_offset`）。
- 更新 `writei()` 越界判断：依赖新 `MAXFILE*BSIZE` 防止写超范围。

```c
if(off + n > MAXFILE*BSIZE)
  return -1;
```

---
## 7. 整体效果与验证路径
1. `bigfile.c` 写入循环现在能成功分配到第 65803 块。
2. 截断/删除时无残留块（通过人工检查或加调试输出）。
3. `mkfs` 生成的初始镜像若包含较大文件不再触发 `file too big` panic。

---
## 8. 回顾：为什么必须减少一个直接块
- 原有 `addrs[NDIRECT+1]` 只够 “12 直接 + 1 一级间接”。
- 想再加入一个二级间接块需再多一个槽位，因此将直接块数从 12 调整为 11，保持数组总长度可控，不扩大磁盘 inode 固定大小结构增量过多。

---
