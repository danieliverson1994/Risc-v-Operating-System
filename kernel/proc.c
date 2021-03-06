#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

extern void* callsigret(void);
extern void* endcallsigret(void);

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl) {
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table at boot time.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->kstack = KSTACK((int) (p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void) {
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void) {
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid() {
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // Initialize handlers to be default
  
  for (int i = 0; i < 32; i++) {
    p->signal_handlers[i] = (void*)SIG_DFL;
  }

  p->signals_mask = 0;
  p->pending_signals = 0;
  p->stopped = 0;
  p->signal_handling = 0;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process,
// with no user memory, but with trampoline pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe just below TRAMPOLINE, for trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// od -t xC initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy init's instructions
  // and data into it.
  uvminit(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  //copy signals_mask and signals_handlers to child proc
  //memmove(&np->signal_handlers, &p->signal_handlers, sizeof(p->signal_handlers));
  for (int j = 0; j < 32; j++) {
    np->signal_handlers[j] = p->signal_handlers[j];
    //debug
    //printf("j: %d, process pid: %d, signal_handler[i]: %d\n",j, np->pid, np->signal_handlers[j]);
  }

  np->signals_mask = p->signals_mask;
  np->pending_signals = 0;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      if(np->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if(np->state == ZOMBIE){
          // Found one.
          pid = np->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || p->killed){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  
  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&p->lock);
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid, int signum)
{
  struct proc *p;
  uint op = 1 << signum;

  if(signum < 0 || signum > 31)
    return -1;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      //debug
      printf("%d: got %d\n",p->pid,signum);
        //printf("inside kill ! i: %d, process pid: %d, signal_handler[i]: %d\n",i, p->pid, p->signal_handlers[i]);

      if(p->killed || p->state==ZOMBIE || p->state==UNUSED){
        return -1;
      }
      //TO ADD - after implemenation of CAS
      /*
      do{
        pending_sigs = p->pending_signals;
      }
      while(!cas(&p->pending_signals, pending_sigs, pending_sigs|op));
      */
      p->pending_signals = p->pending_signals|op;
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

uint
sigprocmask(uint mask)
{
  struct proc *p = myproc();
  uint old_mask = p->signals_mask;
  p->signals_mask = mask & ~(1 << SIGKILL | 1 << SIGSTOP);
  return old_mask;
}

int
sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)
{
  //debug
  printf("act: %d\n", act);
  struct proc *p = myproc();

  if(signum < 0 || signum > 31)
    return -1;

  if(signum == SIGKILL || signum == SIGSTOP)
    return -1;

  if(act == 0) 
    return -1;

  //struct sigaction *var;

  if(act == (void*)SIG_DFL) {
    //debug
    printf("inside SIG_DFL case\n");
    if( oldact != 0)
      memmove(&oldact, &p->signal_handlers[signum], sizeof(void*));
      //copyout(p->pagetable, (uint64)&oldact, (char*)&p->signal_handlers[signum], sizeof(struct sigaction));
    p->signal_handlers[signum] = &act;
  }
  else {
    //debug
    printf("user handler case\n");
    if (oldact != 0) {
      //debut
      printf("memmove to oldact\n");
      memmove(&oldact, &p->signal_handlers[signum], sizeof(void*));
    }
    //copyout(p->pagetable, (uint64)&oldact, (char*)&p->signal_handlers[signum], sizeof(struct sigaction));
    //debug
    //printf("after copyout\n");
    memmove(&p->signal_handlers[signum], &act, sizeof(void*));
    //copyin(p->pagetable, (char*)&p->signal_handlers[signum], (uint64)&act, sizeof(struct sigaction));
    //debug
    //printf("after copyin\n");
  }
  //debug
  //printf("returning 0\n");
  return 0;
}

void
sigret(void)
{
  //debug
  printf("sigret!!!\n");
  printf("inside sigret!!!!!!\n");
  struct proc *p = myproc();
  acquire(&p->lock);
  copyin(p->pagetable, (char*)p->trapframe, (uint64)p->user_tf_backup, sizeof(struct trapframe));
  p->signals_mask = p->signals_mask_backup;
  p->user_tf_backup = 0;
  p->signal_handling = 0;
  release(&p->lock);
  //debug
  printf("end of sigret func\n");
  //return p->trapframe->eax; // ?????
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}

void 
sigcont_func(void)
{
  struct proc *p = myproc();
  //acquire(&p->lock);
  p->stopped = 0;
  //release(&p->lock);
}

void
sigstop_func(void)
{
  struct proc *p = myproc();
  //acquire(&p->lock);
  p->stopped = 1;
  //release(&p->lock);
}

void
sigkill_func(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->killed = 1;
  if(p->state == SLEEPING)
    p->state = RUNNABLE;
  release(&p->lock);
}

int
is_pending_and_not_masked(int signum) {
  struct proc *p = myproc();

  uint64 non_maskable = ~(1 << SIGKILL) & ~(1 << SIGSTOP);
  
  if((p->pending_signals & (1 << signum)) && !((p->signals_mask & non_maskable) & (1 << signum)))
    return 1;
  
  else
    return 0;
  
}

/*
void
usersignalhandler(struct proc *p, int signum) {
  // 1. jesus WHAT
  char *dst=0; // ????
  //uint64 address= p->signal_handlers[signum].sa_handler;
  struct sigaction *handler = p->signal_handlers[signum];
  uint64 address = (uint64)(handler->sa_handler);
  copyin(p->pagetable, dst, address, sizeof(uint64));
  
  //copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
  //2.
  uint64 bakcUpSignalMask = p->signals_mask;
  p->signals_mask= (uint)handler->sigmask;
  //3.
  p->signal_handling = 1;
  //4.
  uint64 local;
  local = p->trapframe->sp;
  local -= sizeof(struct trapframe);
  p->user_tf_backup = (struct trapframe *)local;
  //5. Now you should use the "copyout" function (from kernel to user), to copy the current process trapframe,

  //   to the trapframe backup stack pointer (to reduce its stack pointer at the user space).
  copyout(p->pagetable, )
  // Copy from kernel to user.
  // Copy len bytes from src to virtual address dstva in a given page table.
  // Return 0 on success, -1 on error.

copyout(pagetable_t pagetable, uint64 dstva, char *src, sizeof(struct trapframe));
}

*/


// void 
// call_sigret(void)
// {
// sigret();
// }


void
turnoff_sigbit(struct proc *p, int i) {
  
  //int op = 1 << i;   UNCOMMENT LATER
  //uint pending_sigs = p->pending_signals;   UNCOMMENT LATER
  acquire(&p->lock);
  p->pending_signals = p->pending_signals & ~(1 << i);
  release(&p->lock);
  //TO ADD - after implemenation of CAS
  /*
  while(!cas(&p->pending_signals, pending_sigs, pending_sigs&op)){
    pending_sigs = p->pending_signals;
  }
  */
}
    //p->pending_signals = p->pending_signals & ~(1 << i); 


int
search_cont_signals(void) {
  struct proc *p = myproc();

  if(is_pending_and_not_masked(SIGCONT)) {
    sigcont_func();
    turnoff_sigbit(p, SIGCONT);
    return 1;
  }

  if(is_pending_and_not_masked(SIGKILL)) {
    sigkill_func();
    turnoff_sigbit(p, SIGKILL);
    return 1;
  }

  for(int i = 0; i < 32; i++) {
    if(is_pending_and_not_masked(i)) {
      if(p->signal_handlers[i] == (void*)SIGCONT){
        sigcont_func();
        turnoff_sigbit(p, i);
        return 1;
      }
    }
  }

  return 0;
}

void
signalhandler(void)
{
  struct proc *p = myproc();
  
  if(p == 0)
    return;
  
  if(p->signal_handling)
    return;
  

  //make sure its not a kernel trap???????

  for(int i = 0; i < 32; i++) {
    //printf("i: %d, process pid: %d, signal_handler[i]: %d\n",i, p->pid, p->signal_handlers[i]);

    //int signal_ptr= 1 << i;
    //check p->killed????
    if(p->killed)
      return;

    while(p->stopped) {
      if(search_cont_signals())
      {
        break;
      }
      //yield CPU back to the scheduler
      yield();
    }

    
    if(is_pending_and_not_masked(i))
    {
      // printf("process pid: %d, signal_handler[i]: %d\n", p->pid, p->signal_handlers[i]);
      //printf("%d: handeling %d\n",p->pid,i);
      //printf("signum: %d, in signalhandler, signal_handling is: %d\n", i, p->signal_handling);
      //debug
      if(p->signal_handling)
        return;
      //printf("handler: %d\n", p->signal_handlers[i]);
      //debug
      printf("handle signal: %d\n", i);

      if(p->signal_handlers[i] == (void*)SIG_IGN){
        continue;
      }
      //kernel space handler
      else if(p->signal_handlers[i] == (void*)SIG_DFL)
      { 
      //debug
      // printf("inside kernel space handler\n");
      
      switch (i){
        case SIGSTOP:
          sigstop_func();
          break;
        
        case SIGCONT:
          sigcont_func();
          break;

        case SIGKILL:
          sigkill_func();
          break;
        
        default:
          sigkill_func();
          break;       
        }
      }

      else if(p->signal_handlers[i] == (void*)SIGSTOP){
        sigstop_func();
      }

      else if(p->signal_handlers[i] == (void*)SIGCONT){
        sigcont_func();
      }

      else if(p->signal_handlers[i] == (void*)SIGKILL){
        sigkill_func();
      }

      //User signal handler
      else{ 
        //debug
        // printf("\ncalling to usersignalshandler!\n");
        usersignalhandler(p, i);
      }    
       
    turnoff_sigbit(p, i);

    }
    //p->pending_signals = p->pending_signals & ~(1 << i); 
  }
  
  //release(&p->lock);

}

void
usersignalhandler(struct proc *p, int signum) {
  acquire(&p->lock);
  //debug
  printf("user handeling %d\n",signum);
  //1
    //debug
  //printf("on stage 1\n");
  uint64 dst; 
  struct sigaction *handler = (struct sigaction*) p->signal_handlers[signum];
  copyin(p->pagetable, (char*)&dst, (uint64)&handler->sa_handler, sizeof(void*));
  
  //2
  //debug
  //printf("on stage 2\n");
  //backup mask
  uint sigmask;
  copyin(p->pagetable, (char*)&sigmask, (uint64)&handler->sigmask, sizeof(uint));
  uint backup_mask = sigprocmask(sigmask);
  p->signals_mask_backup = backup_mask;
  
  //3
  //debug
  //printf("on stage 3\n");
  //turn on flag
  //acquire(&p->lock);
  //debug
  //printf("signum: %d, in userhandler, signal_handling is: %d\n", signum, p->signal_handling);
  p->signal_handling = 1;
  //printf("signum: %d, in userhandler, signal_handling is: %d\n", signum, p->signal_handling);

  //release(&p->lock);
 
  //4
    //debug
  //printf("on stage 4\n");
  //reduce the process trapframe stack pointer by the size of trapframe
  uint64 sp_n = p->trapframe->sp - sizeof(struct trapframe);
  p->user_tf_backup = (struct trapframe*)sp_n;
   
  //5 
    //debug
  //printf("on stage 5\n");
  //backup the process trap frame 
  copyout(p->pagetable, (uint64)p->user_tf_backup, (char*)p->trapframe, sizeof(struct trapframe));
  
  //6    
  //debug
  //printf("on stage 6\n");
  p->trapframe->epc = (uint64)dst;
  
  //7
    //debug
  //printf("on stage 7\n");
  int func_size = (endcallsigret - callsigret);
  sp_n -= func_size;
  //reduce the trapframe stack pointer by func size
  p->trapframe->sp = sp_n;
  
  //8
    //debug
  // printf("on stage 8\n");
  //copy call_sigret to the process trapframe stack pointer 
  copyout(p->pagetable, (uint64)(p->trapframe->sp), (char*)&callsigret, func_size); // second arg?????
  
  //9
    //debug
  // printf("on stage 9\n");
  //debug
  printf("inside user handler, signum: %d\n", signum);
  p->trapframe->a0 = signum;
  //put at the process return address register the new trapframe sp
  //debug
  p->trapframe->ra = sp_n;
  //p->signals_mask = backup_mask;
  //p->signal_handling = 1;
  //debug
  // printf("after last stage\n");
  release(&p->lock);
}