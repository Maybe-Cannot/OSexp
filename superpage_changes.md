# xv6 超级页（Superpage）支持修改文档

## 概述
本次修改为 xv6 实现了 2MB 超级页（superpage）分配与映射支持，主要目标是通过预留超级页池解决分配碎片化问题，并保证 `pgtbltest` 测试通过。

---

## 主要修改点

### 1. kernel/kalloc.c
#### 新增/修改内容：
- **超级页池（superfreelist）**：在 `kmem` 结构体中新增 `superfreelist`，用于管理 2MB 超级页。
- **kinit() / freerange()**：在内核初始化时，预留 10 个连续物理超级页（共 20MB），并将其加入 `superfreelist`，剩余内存按页加入普通 freelist。
- **superalloc() / superfree()**：实现超级页分配与释放逻辑，仅从 `superfreelist` 分配/回收超级页。
- **调试信息**：所有调试输出已清除。

#### 相关函数：
- `struct kmem` 新增 `superfreelist`
- `kinit()`
- `freerange()`
- `superalloc()`
- `superfree()`

---

### 2. kernel/vm.c
#### 新增/修改内容：
- **mappages()**：支持将虚拟地址直接映射到物理超级页（Level 1 PTE），用于 2MB 对齐区域。
- **uvmalloc()**：优先使用 `superalloc()` 分配 2MB 对齐的虚拟内存区域。
- **调试信息**：所有调试输出已清除。

#### 相关函数：
- `mappages()`
- `uvmalloc()`

---

### 3. user/pgtbltest.c
#### 新增/修改内容：
- **测试用例**：`superpg_test()` 用于验证超级页分配与映射正确性。
- **调试信息**：所有调试输出已清除。

#### 相关函数：
- `superpg_test()`
- `supercheck()`

---

## 设计与实现说明

### 超级页池预留方案
- 由于普通 freelist 可能碎片化，导致无法分配连续 2MB 物理页。
- 采用方案2：在内核启动时预留 10 个超级页（20MB），专用于超级页分配。
- 超级页分配/释放仅操作 `superfreelist`，不影响普通页分配。

### 页表映射
- 2MB 虚拟地址映射时，直接在页表第一级（Level 1）设置超级页 PTE。
- 普通页仍按 4KB 分配与映射。

### 兼容性
- 普通页分配逻辑未变，兼容原有功能。
- 超级页池大小可根据实际需求调整。

---

## 相关文件与函数列表

| 文件              | 主要修改函数/结构体         | 说明                       |
|-------------------|-----------------------------|----------------------------|
| kernel/kalloc.c   | kmem, kinit, freerange,     | 超级页池管理与分配         |
|                   | superalloc, superfree       |                            |
| kernel/vm.c       | mappages, uvmalloc          | 超级页映射与分配           |
| user/pgtbltest.c  | superpg_test, supercheck    | 超级页分配与映射测试       |

---

## 测试结果
- `make qemu` 后，`pgtbltest` 测试通过，超级页分配与映射功能正常。
- 代码已清理所有调试输出，便于后续维护。

---

## 参考与建议
- 超级页池大小可根据实际内存需求调整。
- 若需动态分配超级页，可进一步优化分配算法。
- 建议后续如需支持更大页（如1GB），可参考本次设计。

---

## 版本信息


如需进一步说明或代码细节，请随时联系。
---

## 相关源码（关键函数实现）

### kernel/kalloc.c
```c
struct run {
	struct run *next;
};

struct {
	struct spinlock lock;
	struct run *freelist;
#ifdef LAB_PGTBL
	struct run *superfreelist;  // 超级页链表
#endif
} kmem;

void kinit()
{
	initlock(&kmem.lock, "kmem");
#ifdef LAB_PGTBL
	kmem.superfreelist = 0;
#endif
	freerange(end, (void*)PHYSTOP);
}

void freerange(void *pa_start, void *pa_end)
{
	char *p;
	p = (char*)PGROUNDUP((uint64)pa_start);
#ifdef LAB_PGTBL
	int super_count = 0;
	char *super_start = (char*)SUPERPGROUNDUP((uint64)p);
	for(; p < super_start && p + PGSIZE <= (char*)pa_end; p += PGSIZE) {
		kfree(p);
	}
	for(; super_count < 10 && p + SUPERPGSIZE <= (char*)pa_end; 
			p += SUPERPGSIZE, super_count++) {
		struct run *r = (struct run*)p;
		r->next = kmem.superfreelist;
		kmem.superfreelist = r;
	}
#endif
	for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
		kfree(p);
}

#ifdef LAB_PGTBL
void* superalloc(void) {
	struct run *r;
	acquire(&kmem.lock);
	r = kmem.superfreelist;
	if(r) {
		kmem.superfreelist = r->next;
	}
	release(&kmem.lock);
	if(r) {
		memset((char*)r, 5, SUPERPGSIZE);
	}
	return (void*)r;
}

void superfree(void *pa)
{
	struct run *r;
	if(((uint64)pa % SUPERPGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
		panic("superfree");
	memset(pa, 1, SUPERPGSIZE);
	r = (struct run*)pa;
	acquire(&kmem.lock);
	r->next = kmem.superfreelist;
	kmem.superfreelist = r;
	release(&kmem.lock);
}
#endif
```

### kernel/vm.c
```c
int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
	uint64 a, last;
	pte_t *pte;
	if(size == 0)
		panic("mappages: size");
	if((va % PGSIZE) != 0)
		panic("mappages: va not aligned");
	if((size % PGSIZE) != 0)
		panic("mappages: size not aligned");
#ifdef LAB_PGTBL
	if(size == SUPERPGSIZE){
		if((va % SUPERPGSIZE) != 0)
			panic("mappages: superpage va not aligned");
		if((pa % SUPERPGSIZE) != 0)
			panic("mappages: superpage pa not aligned");
		pte_t *l2_pte = &pagetable[PX(2, va)];
		pagetable_t l1;
		if(*l2_pte & PTE_V) {
			l1 = (pagetable_t)PTE2PA(*l2_pte);
		} else {
			l1 = (pagetable_t)kalloc();
			if(l1 == 0)
				return -1;
			memset(l1, 0, PGSIZE);
			*l2_pte = PA2PTE((uint64)l1) | PTE_V;
		}
		pte_t *l1_pte = &l1[PX(1, va)];
		if(*l1_pte & PTE_V)
			panic("mappages: superpage remap");
		uint64 pte_val = PA2PTE(pa) | perm | PTE_V;
		*l1_pte = pte_val;
		return 0;
	}
#endif
	a = va;
	last = va + size - PGSIZE;
	for(;;){
		if((pte = walk(pagetable, a, 1)) == 0)
			return -1;
		if(*pte & PTE_V)
			panic("mappages: remap");
		*pte = PA2PTE(pa) | perm | PTE_V;
		if(a == last)
			break;
		a += PGSIZE;
		pa += PGSIZE;
	}
	return 0;
}

uint64 uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
	char *mem;
	uint64 a;
	int sz;
	if(newsz < oldsz)
		return oldsz;
	oldsz = PGROUNDUP(oldsz);
	for(a = oldsz; a < newsz; a += sz){
		sz = PGSIZE;
#ifdef LAB_PGTBL
		if((a % SUPERPGSIZE == 0) && (newsz - a >= SUPERPGSIZE)) {
			mem = superalloc();
			if(mem != 0) {
				sz = SUPERPGSIZE;
#ifndef LAB_SYSCALL
				memset(mem, 0, sz);
#endif
				if(mappages(pagetable, a, sz, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
					superfree(mem);
					uvmdealloc(pagetable, a, oldsz);
					return 0;
				}
				continue;
			}
		}
#endif
		mem = kalloc();
		if(mem == 0){
			uvmdealloc(pagetable, a, oldsz);
			return 0;
		}
#ifndef LAB_SYSCALL
		memset(mem, 0, PGSIZE);
#endif
		if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
			kfree(mem);
			uvmdealloc(pagetable, a, oldsz);
			return 0;
		}
	}
	return newsz;
}
```

### user/pgtbltest.c
```c
void supercheck(uint64 s)
{
	pte_t last_pte = 0;
	for (uint64 p = s;  p < s + 512 * PGSIZE; p += PGSIZE) {
		pte_t pte = (pte_t) pgpte((void *) p);
		if(pte == 0)
			err("no pte");
		if ((uint64) last_pte != 0 && pte != last_pte) {
				err("pte different");
		}
		if((pte & PTE_V) == 0 || (pte & PTE_R) == 0 || (pte & PTE_W) == 0){
			err("pte wrong");
		}
		last_pte = pte;
	}
	for(int i = 0; i < 512 * PGSIZE; i += PGSIZE){
		*(int*)(s+i) = i;
	}
	for(int i = 0; i < 512 * PGSIZE; i += PGSIZE){
		if(*(int*)(s+i) != i)
			err("wrong value");
	}
}

void superpg_test()
{
	int pid;
	printf("superpg_test starting\n");
	testname = "superpg_test";
	char *end = sbrk(N);
	if (end == 0 || end == (char*)0xffffffffffffffff)
		err("sbrk failed");
	uint64 s = SUPERPGROUNDUP((uint64) end);
	supercheck(s);
	if((pid = fork()) < 0) {
		err("fork");
	} else if(pid == 0) {
		supercheck(s);
		exit(0);
	} else {
		int status;
		wait(&status);
		if (status != 0) {
			exit(0);
		}
	}
	printf("superpg_test: OK\n");  
}
```
