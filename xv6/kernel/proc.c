#include "types.h"
#include "defs.h"
#include "param.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
#include "pstat.h"
struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);
int time_slice[4]={TIME_SLICE_0,TIME_SLICE_1,TIME_SLICE_2,TIME_SLICE_3};

struct pqueue{
  int len;
  struct proc* qproc[NPROC];
};

struct pqueue* pque[4] = {
  &(struct pqueue){ .len = 0, .qproc = { NULL } },
  &(struct pqueue){ .len = 0, .qproc = { NULL } },
  &(struct pqueue){ .len = 0, .qproc = { NULL } },
  &(struct pqueue){ .len = 0, .qproc = { NULL } }
};

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

int
getpinfo(struct pstat* p) {
  struct proc* cp;
  acquire(&ptable.lock);
  for(int i = 0; i < NPROC; ++i) {
    cp = &ptable.proc[i];
    p->inuse[i] = cp->state == UNUSED ? 0 : 1;
    p->pid[i] = cp->pid;
    p->priority[i] = cp->priority;
    p->state[i]=cp->state;
    for(int j=0;j<=PRIORITY_HIGH;j++){
      p->ticks[i][j]=cp->ticks[j];
      p->wait_ticks[i][j] = cp->wait_ticks[j];
    }
  }
  release(&ptable.lock);
  return 0;
}

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
  /* alloc to the queue */
  p->priority = PRIORITY_HIGH;
  for(int i=0;i<=PRIORITY_HIGH;i++){
    p->ticks[i] = 0;
    p->wait_ticks[i]=0;
  }
  int len = pque[PRIORITY_HIGH]->len;
  pque[PRIORITY_HIGH]->qproc[len] = p;
  pque[PRIORITY_HIGH]->len++;
  release(&ptable.lock);

  // Allocate kernel stack if possible.
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

// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];
  
  p = allocproc();
  acquire(&ptable.lock);
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

  p->state = RUNNABLE;
  release(&ptable.lock);
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

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(proc->ofile[i])
      np->ofile[i] = filedup(proc->ofile[i]);
  np->cwd = idup(proc->cwd);
 
  pid = np->pid;
  np->state = RUNNABLE;
  safestrcpy(np->name, proc->name, sizeof(proc->name));
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

  iput(proc->cwd);
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

//when a process runs out of its time slice, it should be downgraded to the lower priority
//p is the process to be downgraded
//idx is the index of p's position in its priority queue
void
downgrade(struct proc* p,int idx){
  int lev=p->priority;
  if (lev != PRIORITY_BOTTOM)
  {
    if (p->ticks[lev] >= time_slice[lev])
    {
      /* downgrade*/
      p->priority--;
      pque[lev - 1]->qproc[pque[lev - 1]->len] = p;
      pque[lev - 1]->len++;
      for (int k = idx; k < pque[lev]->len - 1; k++)
        pque[lev]->qproc[k] = pque[lev]->qproc[k + 1];
      pque[lev]->qproc[pque[lev]->len] = NULL;
      pque[lev]->len--;
    }
  }
}
//If a process has waited 10x the time slice in its current priority level, it is raised to the next higher priority level at this time
void
priorityboost(){
  struct proc *starvp;
  for (starvp = ptable.proc; starvp < &ptable.proc[NPROC]; starvp++){//Iterate through all processes in ptable
    if (starvp->state != RUNNABLE)
      continue;
    int lev=starvp->priority;
    starvp->wait_ticks[lev]++;

    /* check starv */
    if (starvp->wait_ticks[starvp->priority] >= (time_slice[starvp->priority] * 10)){
      starvp->wait_ticks[starvp->priority] = 0;
      if (starvp->priority != 3){
        /* start upgrade p */
        int lev = starvp->priority;
        int idx=0;
        while(pque[lev]->qproc[idx]!=starvp)idx++;
        for (int k = idx; k < pque[lev]->len - 1; k++)
          pque[lev]->qproc[k] = pque[lev]->qproc[k + 1];
        pque[lev]->qproc[pque[lev]->len] = NULL;
        pque[lev]->len--;

        starvp->priority++;
        pque[lev + 1]->qproc[pque[lev + 1]->len] = starvp;
        pque[lev + 1]->len++;
        /* end upgrade p */
      }
    }
  }
}
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  for (;;)
  {
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    
    //a process queue with a priority of 3,2,1 uses a round-robin to schedule all processes at that priority
    for (int lev = PRIORITY_HIGH; lev >0; lev--){
      if (pque[lev]->len > 0)
      {
        /* j = p index of the queue */
        for (int j = 0; j < pque[lev]->len; j++)
        {
          if (pque[lev]->qproc[j]->state != RUNNABLE)
            continue;
          p = pque[lev]->qproc[j];
          p->wait_ticks[p->priority] = 0;
          if(p->ticks[lev]>=time_slice[lev]){
            p->ticks[lev]=0;
          }
          while(p->ticks[lev]<time_slice[lev]&&p->state==RUNNABLE){
            p->ticks[lev]++;
            proc=p;
            switchuvm(p);
            p->state = RUNNING;
            priorityboost();
            swtch(&cpu->scheduler, proc->context);
            switchkvm();
            proc=0;
          }
          downgrade(p,j);
        }
      }
    }
    //priority 0 processes are scheduled on a FIFO
    //the scheduled process should continue to exec until it finished or exit
    for(int j=0;j<pque[0]->len;j++){
      if (pque[0]->qproc[j]->state != RUNNABLE)
        continue;
      p = pque[0]->qproc[j];
      p->wait_ticks[p->priority] = 0;
      while(p->state==RUNNABLE){
        p->ticks[p->priority]++;
        proc=p;
        switchuvm(p);
        p->state = RUNNING;
        priorityboost();
        swtch(&cpu->scheduler, proc->context);
        switchkvm();
        proc=0;
      }
    }
    release(&ptable.lock);
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

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);
  
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

