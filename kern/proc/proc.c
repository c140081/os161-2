/*
 * Copyright (c) 2013
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Process support.
 *
 * There is (intentionally) not much here; you will need to add stuff
 * and maybe change around what's already present.
 *
 * p_lock is intended to be held when manipulating the pointers in the
 * proc structure, not while doing any significant work with the
 * things they point to. Rearrange this (and/or change it to be a
 * regular lock) as needed.
 *
 * Unless you're implementing multithreaded user processes, the only
 * process that will have more than one thread is the kernel process.
 */

#include <types.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <vnode.h>
#include <vfs.h>
#include <synch.h>
#include <kern/fcntl.h>
#include <array.h>

/*
 * The process for the kernel; this holds all the kernel-only threads.
 */
struct proc *kproc;

/*
 * Mechanism for making the kernel menu thread sleep while processes are running
 */
#ifdef UW
/* count of the number of processes, excluding kproc */
static unsigned int proc_count;
/* provides mutual exclusion for proc_count */
/* it would be better to use a lock here, but we use a semaphore because locks are not implemented in the base kernel */
static struct semaphore *proc_count_mutex;
/* used to signal the kernel menu thread when there are no processes */
struct semaphore *no_proc_sem;
#endif  // UW

// The complete array of all active processes
// Processes are active if they have not exited or if their parent has not existed
struct array *allprocs;

// PIDs will be > 0
pid_t base_pid = 0;

/**
	Returns the index of the given process in the processes array
	Binary search implementation
*/
unsigned procarray_proc_index_by_pid(struct array *procs, pid_t pid) {
	unsigned low = 0;
	unsigned high = array_num(procs) - 1;

	while (high >= low) {
		unsigned probe = (high + low) / 2;
		struct proc * p = array_get(procs, probe);
		if (pid == p->p_id) {
			return probe;
		} else if (pid < p->p_id) {
			high = probe - 1;
		} else {
			low = probe + 1;
		}
	}

	// No process found for this pid
	return -1;
}

/**
	proc_by_pid returns the process for a given PID by performing a binary
	search on the processes array.
*/
struct proc * procarray_proc_by_pid(struct array *procs, pid_t pid) {
	return array_get(procs, procarray_proc_index_by_pid(procs, pid));
}

struct proc * procarray_allprocs_proc_by_pid(pid_t pid) {
	return procarray_proc_by_pid(allprocs, pid);
}

/**
	Adds a newly created process to the end of the processes array
	To be called by proc_create
*/
void procarray_add_proc(struct array *procs, struct proc *p) {
	array_add(procs, p, NULL);
}

void procarray_allprocs_add_proc(struct proc *p) {
	if (allprocs == NULL) {
		allprocs = array_create();
		array_init(allprocs);
	}
	procarray_add_proc(allprocs, p);
}

/**
	To be called by proc_destroy
*/
void procarray_remove_proc(struct array *procs, pid_t pid) {
	array_remove(procs, procarray_proc_index_by_pid(procs, pid));
}

void procarray_allprocs_remove_proc(pid_t pid) {
	procarray_remove_proc(allprocs, pid);

	// Deinit the processes array
	if (array_num(allprocs) == 0) {
		array_cleanup(allprocs);
		array_destroy(allprocs);
		allprocs = NULL;
	}
}

/**
	Generates a unique PID for a process.

	TODO: I didn't have time to get to this by refactor this to allow
	(potentially) unlimited different processes to run by allowing recycling of
	process IDs that have exited and been fully removed.

	We would have to generate the PID by finding the largest array index `i` in
	allprocesses whose process ID is <= i (specified binary search) then insert
	into the allprocesses array when we create the new process in that order (to
	maintion ordering by PID).
*/
pid_t gen_pid() {
	return ++base_pid;
}

/*
 * Create a proc structure.
 */
static struct proc * proc_create(const char *name) {

	struct proc *proc;

	proc = kmalloc(sizeof(*proc));
	if (proc == NULL) {
		return NULL;
	}
	proc->p_name = kstrdup(name);
	if (proc->p_name == NULL) {
		kfree(proc);
		return NULL;
	}

	threadarray_init(&proc->p_threads);
	spinlock_init(&proc->p_lock);

	/* VM fields */
	proc->p_addrspace = NULL;

	/* VFS fields */
	proc->p_cwd = NULL;

#ifdef UW
	proc->console = NULL;
#endif // UW

	// Added for A2

	// Generate a process ID and add it to the processes array
	proc->p_id = gen_pid();
	proc->p_did_exit = false;
	proc->p_exitcode = 0;

	proc->p_exit_lk = lock_create("p_exit_lk");
	if (proc->p_exit_lk == NULL) {
		kfree(proc->p_name);
		kfree(proc);
		return NULL;
	}
	proc->p_wait_lk = lock_create("p_wait_lk");
	if (proc->p_exit_lk == NULL) {
		lock_destroy(proc->p_exit_lk);
		kfree(proc->p_name);
		kfree(proc);
		return NULL;
	}

	proc->p_wait_cv = cv_create("p_wait_cv");
	if (proc->p_exit_lk == NULL) {
		lock_destroy(proc->p_wait_lk);
		lock_destroy(proc->p_exit_lk);
		kfree(proc->p_name);
		kfree(proc);
		return NULL;
	}

	proc->p_children = *array_create();
	array_init(&proc->p_children); // initialize the children

	// Process created successfully, add it to the array of all processes
	procarray_allprocs_add_proc(proc);

	return proc;
}

/*
 * Destroy a proc structure.
 */
void proc_destroy(struct proc *proc) {
	/*
         * note: some parts of the process structure, such as the address space,
         *  are destroyed in sys_exit, before we get here
         *
         * note: depending on where this function is called from, curproc may not
         * be defined because the calling thread may have already detached itself
         * from the process.
	 */

	KASSERT(proc != NULL);
	KASSERT(proc != kproc);

	// Remove the process from the global process array.
	procarray_allprocs_remove_proc(proc->p_id);

	/*
	 * We don't take p_lock in here because we must have the only
	 * reference to this structure. (Otherwise it would be
	 * incorrect to destroy it.)
	 */

	/* VFS fields */
	if (proc->p_cwd) {
		VOP_DECREF(proc->p_cwd);
		proc->p_cwd = NULL;
	}


#ifndef UW  // in the UW version, space destruction occurs in sys_exit, not here

	if (proc->p_addrspace) {
		/*
		 * In case p is the currently running process (which
		 * it might be in some circumstances, or if this code
		 * gets moved into exit as suggested above), clear
		 * p_addrspace before calling as_destroy. Otherwise if
		 * as_destroy sleeps (which is quite possible) when we
		 * come back we'll be calling as_activate on a
		 * half-destroyed address space. This tends to be
		 * messily fatal.
		 */
		struct addrspace *as;

		as_deactivate();
		as = curproc_setas(NULL);
		as_destroy(as);
	}
#endif // UW

#ifdef UW
	if (proc->console) {
	  vfs_close(proc->console);
	}
#endif // UW

	threadarray_cleanup(&proc->p_threads);
	spinlock_cleanup(&proc->p_lock);

	// Added for A2
	array_cleanup(&proc->p_children);
	lock_destroy(proc->p_exit_lk);
	lock_destroy(proc->p_wait_lk);
	cv_destroy(proc->p_wait_cv);

	kfree(proc->p_name);
	kfree(proc);

#ifdef UW
	/* decrement the process count */
	/* note: kproc is not included in the process count, but proc_destroy
	   is never called on kproc (see KASSERT above), so we're OK to decrement
	   the proc_count unconditionally here */
	P(proc_count_mutex);
	KASSERT(proc_count > 0);
	proc_count--;
	/* signal the kernel menu thread if the process count has reached zero */
	if (proc_count == 0) {
	  V(no_proc_sem);
	}
	V(proc_count_mutex);
#endif // UW


}

/*
 * Create the process structure for the kernel.
 */
void proc_bootstrap(void) {

	kproc = proc_create("[kernel]");
	if (kproc == NULL) {
		panic("proc_create for kproc failed\n");
}
#ifdef UW
	proc_count = 0;
	proc_count_mutex = sem_create("proc_count_mutex",1);
	if (proc_count_mutex == NULL) {
		panic("could not create proc_count_mutex semaphore\n");
	}
	no_proc_sem = sem_create("no_proc_sem",0);
	if (no_proc_sem == NULL) {
		panic("could not create no_proc_sem semaphore\n");
	}
#endif // UW
}

/*
 * Create a fresh proc for use by runprogram.
 *
 * It will have no address space and will inherit the current
 * process's (that is, the kernel menu's) current directory.
 */
struct proc * proc_create_runprogram(const char *name) {
	struct proc *proc;
	char *console_path;

	proc = proc_create(name);
	if (proc == NULL) {
		return NULL;
	}

#ifdef UW
	/* open the console - this should always succeed */
	console_path = kstrdup("con:");
	if (console_path == NULL) {
	  panic("unable to copy console path name during process creation\n");
	}
	if (vfs_open(console_path,O_WRONLY,0,&(proc->console))) {
	  panic("unable to open the console during process creation\n");
	}
	kfree(console_path);
#endif // UW

	/* VM fields */

	proc->p_addrspace = NULL;

	/* VFS fields */

#ifdef UW
	/* we do not need to acquire the p_lock here, the running thread should
           have the only reference to this process */
        /* also, acquiring the p_lock is problematic because VOP_INCREF may block */
	if (curproc->p_cwd != NULL) {
		VOP_INCREF(curproc->p_cwd);
		proc->p_cwd = curproc->p_cwd;
	}
#else // UW
	spinlock_acquire(&curproc->p_lock);
	if (curproc->p_cwd != NULL) {
		VOP_INCREF(curproc->p_cwd);
		proc->p_cwd = curproc->p_cwd;
	}
	spinlock_release(&curproc->p_lock);
#endif // UW

#ifdef UW
	/* increment the count of processes */
        /* we are assuming that all procs, including those created by fork(),
           are created using a call to proc_create_runprogram  */
	P(proc_count_mutex);
	proc_count++;
	V(proc_count_mutex);
#endif // UW

	return proc;
}

/*
 * Add a thread to a process. Either the thread or the process might
 * or might not be current.
 */
int proc_addthread(struct proc *proc, struct thread *t) {
	int result;

	KASSERT(t->t_proc == NULL);

	spinlock_acquire(&proc->p_lock);
	result = threadarray_add(&proc->p_threads, t, NULL);
	spinlock_release(&proc->p_lock);
	if (result) {
		return result;
	}
	t->t_proc = proc;
	return 0;
}

/*
 * Remove a thread from its process. Either the thread or the process
 * might or might not be current.
 */
void proc_remthread(struct thread *t) {
	struct proc *proc;
	unsigned i, num;

	proc = t->t_proc;
	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	/* ugh: find the thread in the array */
	num = threadarray_num(&proc->p_threads);
	for (i=0; i<num; i++) {
		if (threadarray_get(&proc->p_threads, i) == t) {
			threadarray_remove(&proc->p_threads, i);
			spinlock_release(&proc->p_lock);
			t->t_proc = NULL;
			return;
		}
	}
	/* Did not find it. */
	spinlock_release(&proc->p_lock);
	panic("Thread (%p) has escaped from its process (%p)\n", t, proc);
}

/*
 * Fetch the address space of the current process. Caution: it isn't
 * refcounted. If you implement multithreaded processes, make sure to
 * set up a refcount scheme or some other method to make this safe.
 */
struct addrspace * curproc_getas(void) {
	struct addrspace *as;
#ifdef UW
        /* Until user processes are created, threads used in testing
         * (i.e., kernel threads) have no process or address space.
         */
	if (curproc == NULL) {
		return NULL;
	}
#endif

	spinlock_acquire(&curproc->p_lock);
	as = curproc->p_addrspace;
	spinlock_release(&curproc->p_lock);
	return as;
}

/*
 * Change the address space of the current process, and return the old
 * one.
 */
struct addrspace * curproc_setas(struct addrspace *newas) {
	struct addrspace *oldas;
	struct proc *proc = curproc;

	spinlock_acquire(&proc->p_lock);
	oldas = proc->p_addrspace;
	proc->p_addrspace = newas;
	spinlock_release(&proc->p_lock);
	return oldas;
}
