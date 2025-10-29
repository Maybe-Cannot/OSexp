// Mutual exclusion lock.
struct spinlock {
  uint locked;       // Is the lock held?

  // For debugging:
  char *name;        // Name of lock.
  struct cpu *cpu;   // The cpu holding the lock.
#ifdef LAB_LOCK
  int nts;
  int n;
#endif
};


// 基于自旋锁实现信号量
struct semaphore {
    struct spinlock lock;
    int value;
};

// 基于信号量实现读写锁
struct rwlock {
    struct semaphore mutexReadCount;
    struct semaphore mutexWriteCount;
    struct semaphore r;
    struct semaphore w;
    int readcount;
    int writecount;
};
