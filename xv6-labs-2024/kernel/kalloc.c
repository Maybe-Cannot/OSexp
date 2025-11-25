// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

// 物理页引用计数：用于实现写时复制。
// 只跟踪从KERNBASE开始的实际可分配物理页。
static struct {
  struct spinlock lock;
  int refcnt[(PHYSTOP - KERNBASE) / PGSIZE];
} kref;

static inline int
pa2index(uint64 pa)
{
  return (pa - KERNBASE) / PGSIZE;
}

void incref(uint64 pa)
{
  acquire(&kref.lock);
  int idx = pa2index(pa);
  if(idx < 0 || idx >= (int)NELEM(kref.refcnt))
    panic("incref range");
  kref.refcnt[idx]++;
  release(&kref.lock);
}

// decref：返回是否已经释放（1表示释放发生）。
int decref(uint64 pa)
{
  int freed = 0;
  acquire(&kref.lock);
  int idx = pa2index(pa);
  if(idx < 0 || idx >= (int)NELEM(kref.refcnt))
    panic("decref range");
  if(kref.refcnt[idx] <= 0)
    panic("decref underflow");
  kref.refcnt[idx]--;
  if(kref.refcnt[idx] == 0){
    // 计数归零，释放物理页。
    freed = 1;
  }
  release(&kref.lock);
  if(freed)
    kfree((void*)pa);
  return freed;
}

int refcount(uint64 pa)
{
  acquire(&kref.lock);
  int idx = pa2index(pa);
  if(idx < 0 || idx >= (int)NELEM(kref.refcnt)){
    release(&kref.lock);
    return -1;
  }
  int r = kref.refcnt[idx];
  release(&kref.lock);
  return r;
}

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&kref.lock, "kref");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);

  // 重置引用计数（供非COW路径直接调用kfree的情况，例如页表页）。
  acquire(&kref.lock);
  int idx = pa2index((uint64)pa);
  if(idx >= 0 && idx < (int)NELEM(kref.refcnt))
    kref.refcnt[idx] = 0;
  release(&kref.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  if(r){
    acquire(&kref.lock);
    int idx = pa2index((uint64)r);
    if(idx < 0 || idx >= (int)NELEM(kref.refcnt))
      panic("kalloc idx");
    if(kref.refcnt[idx] != 0){
      // 正常情况下应为0，若不为0说明重复分配。
      panic("kalloc refcnt not zero");
    }
    kref.refcnt[idx] = 1;
    release(&kref.lock);
  }
  return (void*)r;
}
