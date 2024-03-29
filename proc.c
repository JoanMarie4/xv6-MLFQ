#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

// CS350 which scheduler to use: 0 for default RR, and 1 for MLFQ
int sched_policy = 1;
// CS350 CPU/process project
int sched_trace_enabled = 1;

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

int RUNNING_THRESHOLD = 2;
int WAITING_THRESHOLD = 4;


static struct proc *initproc;

int nextpid = 1;


extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);



void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;
  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  p->waiting_tick = 0;
  p->running_tick = 0;
  p->pin = 1;
  p->queue = 0;
  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;
  
  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;
  
  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];
  
  p = allocproc(); //initialize the in-kernel PCB

  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->waiting_tick = 0;
  p->running_tick = 0;
  p->pin = 1;
  p->queue = 0;

  p->state = RUNNABLE;
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  
  sz = proc->sz;
  if(n > 0){
    if((sz = allocuvm(proc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(proc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  proc->sz = sz;
  switchuvm(proc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;

  // Allocate process.
  if((np = allocproc()) == 0)
    return -1;

  // Copy process state from p.
  if((np->pgdir = copyuvm(proc->pgdir, proc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = proc->sz;
  np->parent = proc;
  *np->tf = *proc->tf;

  np->waiting_tick = 0;
  np->running_tick = 0;
  np->pin = 1;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(proc->ofile[i])
      np->ofile[i] = filedup(proc->ofile[i]);
  np->cwd = idup(proc->cwd);

  safestrcpy(np->name, proc->name, sizeof(proc->name));
 
  pid = np->pid;

  // lock to force the compiler to emit the np->state write last.
  acquire(&ptable.lock);
  np->state = RUNNABLE;
  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *p;
  int fd;

  if(proc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(proc->ofile[fd]){
      fileclose(proc->ofile[fd]);
      proc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(proc->cwd);
  end_op();
  proc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(proc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == proc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  proc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;

  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for zombie children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != proc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.

        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->state = UNUSED;
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || proc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(proc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.

// CS350: xv6's default RR scheduler
int sched1(void)
{
  struct proc *p;
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      proc = p;
      switchuvm(p);
      p->state = RUNNING;
      swtch(&cpu->scheduler, proc->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      proc = 0;
  }
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state == RUNNABLE)
	return 1;
  }
  return 0;
  
}


// CS350: Multi-level queue feedack scheduling
int sched2(void)
{
  struct proc *p;
  int queue0 = 0;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if (p->state == RUNNABLE && p->queue == 0) {
      queue0 = 1;
      break;
    }
  }
  if (queue0) {
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;
      if (p->queue == 1) {
        // cprintf("PID=%d is in q1 so waiting_tick is incremented\n", p->pid);
        p->waiting_tick++;
        if (p->waiting_tick > WAITING_THRESHOLD) {
          // cprintf("PROMOTE PID=%d:\tRT: %d\tWT: %d\n", p->pid, RUNNING_THRESHOLD, WAITING_THRESHOLD);
          p->queue = 0;
          p->running_tick = 0;
          p->waiting_tick = 0;
        }
        continue;
      }

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      // cprintf("[%d-q%d-p%d]", p->pid, p->queue, p->pin);
      p->running_tick++;
      proc = p;
      switchuvm(p);
      p->state = RUNNING;
      swtch(&cpu->scheduler, proc->context);
      switchkvm();

      if (p->running_tick > RUNNING_THRESHOLD && p->pid != 1 && p->pid != 2 && p->pin > 0) {
        // cprintf("DEMOTE PID=%d:\tRT: %d\tWT: %d\n", p->pid, RUNNING_THRESHOLD, WAITING_THRESHOLD);
        p->queue = 1;
        p->running_tick = 0;
        p->waiting_tick = 0;
      }

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      proc = 0;
    }
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
        if(p->state == RUNNABLE)
          return 1;
    }
  } else {
    struct proc *p;
    struct proc *max_waiting_process;
    int at_least_one_proc = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state == RUNNABLE && p->queue==1){
        max_waiting_process = p;
        at_least_one_proc = 1;
        break;
      }
    }
    if (!at_least_one_proc) return 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if (p->state != RUNNABLE || p->queue == 0 || max_waiting_process == p) continue;
      if(p->waiting_tick > max_waiting_process->waiting_tick){
        max_waiting_process->waiting_tick++;
        if(max_waiting_process->waiting_tick > WAITING_THRESHOLD){
          // cprintf("PROMOTE PID=%d:\tRT: %d\tWT: %d\n", max_waiting_process->pid, RUNNING_THRESHOLD, WAITING_THRESHOLD);
          max_waiting_process->running_tick = 0;
          max_waiting_process->waiting_tick = 0;
          max_waiting_process->queue = 0;
        }
        max_waiting_process = p;
      }else{
        p->waiting_tick++;
        if(p->waiting_tick > WAITING_THRESHOLD){
          // cprintf("PROMOTE PID=%d:\tRT: %d\tWT: %d\n", p->pid, RUNNING_THRESHOLD, WAITING_THRESHOLD);
          p->running_tick = 0;
          p->waiting_tick = 0;
          p->queue = 0;
        }
      }
    }
    // cprintf("[%d-q%d-p%d]", max_waiting_process->pid, max_waiting_process->queue, max_waiting_process->pin);
    // cprintf("[%d-wt%d]", max_waiting_process->pid, max_waiting_process->waiting_tick);
    proc = max_waiting_process;
    switchuvm(max_waiting_process);
    max_waiting_process->state = RUNNING;
    swtch(&cpu->scheduler, proc->context);
    switchkvm();
    proc = 0;

    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
        if(p->state == RUNNABLE) return 1;
    }
  }
  return 0;
}


// CS350: Modify existing scheduler to support two schedulers
void
scheduler(void)
{
  //struct proc *p;
  int ran = 0; // CS350: to solve the 100%-CPU-utilization-when-idling problem

  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    if (sched_policy)
    	ran = sched2();    //MLFQ
    else
	ran = sched1();    //default RR
    release(&ptable.lock);

    if (ran == 0){
        halt();
    }
  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state.
void
sched(void)
{
  int intena;

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(cpu->ncli != 1)
    panic("sched locks");
  if(proc->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = cpu->intena;

  //CS350 tracing function
  if ( sched_trace_enabled && 
	proc && 
	proc->pid != 1
	&& (proc->pid != 2))
   	cprintf("[%d]", proc->pid);

  swtch(&proc->context, cpu->scheduler);
  cpu->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  proc->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

int setpriority(int pid, int priority){
  acquire(&ptable.lock);
  struct proc *p;
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if (p->pid == pid) {
      p->pin = priority;
      if (priority == 0) {
        p->queue = 0;
        // p->waiting_tick = 0;
        // p->running_tick = 0;
      }
    }
  }
  release(&ptable.lock);
  return 0;
}

int setrunningticks(int new_ticks){
  if(new_ticks < 0)
    return -1;
  RUNNING_THRESHOLD = new_ticks;
  return 0;
}

int setwaitingticks(int new_ticks){
  if(new_ticks < 0)
    return -1;
  WAITING_THRESHOLD = new_ticks;
  return 0;
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot 
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }
  
  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  if(proc == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }

  // Go to sleep.
  proc->chan = chan;
  proc->state = SLEEPING;
  sched();

  // Tidy up.
  proc->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];
  
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}
