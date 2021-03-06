#include "sched.h"
#include "schedproc.h"

#include <assert.h>
#include <minix/com.h>
#include <timers.h>
#include <machine/archtypes.h>
#include <sys/resource.h> /* for PRIO_MAX & PRIO_MIN */
#include "kernel/proc.h" /* for queue constants */

#define SCHEDULE_CHANGE_PRIO	0x1
#define SCHEDULE_CHANGE_QUANTUM	0x2
#define SCHEDULE_CHANGE_CPU	0x4

#define SCHEDULE_CHANGE_ALL	(	\
		SCHEDULE_CHANGE_PRIO	|	\
		SCHEDULE_CHANGE_QUANTUM	|	\
		SCHEDULE_CHANGE_CPU		\
		)

#define CPU_DEAD	-1

#define DEFAULT_USER_TIME_SLICE 200
#define INC_PER_QUEUE 10

unsigned cpu_proc[CONFIG_MAX_CPUS];

#define cpu_is_available(c)	(cpu_proc[c] >= 0)
#define is_system_proc(p)	((p)->parent == RS_PROC_NR)

extern "C" int call_minix_sys_schedule(endpoint_t proc_ep, int priority, int quantum, int cpu);
extern "C" int call_minix_sys_schedctl(unsigned flags, endpoint_t proc_ep, int priority, int quantum, int cpu);
extern "C" int accept_message(message *m_ptr);
extern "C" int no_sys(int who_e, int call_nr);
int sched_isokendpt(int endpoint, int *proc);
int sched_isemptyendpt(int endpoint, int *proc);

void Schedproc::pick_cpu()
{
#ifdef CONFIG_SMP
	unsigned cpu, c;
	unsigned cpu_load = (unsigned) -1;

	if (machine.processors_count == 1) {
		this->cpu = machine.bsp_id;
		return;
	}

	if (is_system_proc(this)) {
		this->cpu = machine.bsp_id;
		return;
	}

	cpu = machine.bsp_id;
	for (c = 0; c < machine.processors_count; c++) {
		if (!cpu_is_available(c))
			continue;
		if (c != machine.bsp_id && cpu_load > cpu_proc[c]) {
			cpu_load = cpu_proc[c];
			cpu = c;
		}
	}
	this->cpu = cpu;
	cpu_proc[cpu]++;
#else
	this->cpu = 0;
#endif
}

extern "C" int do_noquantum(message *m_ptr) 
{
	Schedproc *rmp;
	int rv, proc_nr_n;
	unsigned ipc, burst, queue_bump;
	short load;

	if (sched_isokendpt(m_ptr->m_source, &proc_nr_n) != OK) {
		printf("SCHED: WARNING: got an invalid endpoint in OOQ msg %u.\n",m_ptr->m_source);
		return EBADEPT;
	}

	rmp = &schedproc[proc_nr_n];

	ipc = (unsigned)m_ptr->SCHEDULING_ACNT_IPC_ASYNC + (unsigned)m_ptr->SCHEDULING_ACNT_IPC_SYNC + 1;

	load = m_ptr->SCHEDULING_ACNT_CPU_LOAD;
	
	burst = (rmp->time_slice * 1000 / ipc) / 100;
 
	burst = rmp->burst_smooth(burst);

	queue_bump = burst/INC_PER_QUEUE;

	if (rmp->max_priority + queue_bump > MIN_USER_Q) {
		queue_bump = MIN_USER_Q - rmp->max_priority;
	}

	rmp->priority = rmp->max_priority + queue_bump;
	rmp->time_slice = rmp->base_time_slice + 2 * queue_bump * (rmp->base_time_slice/10);

	if ((rv = rmp->schedule_process(SCHEDULE_CHANGE_PRIO | SCHEDULE_CHANGE_QUANTUM)) != OK) {
		return rv;
	}
	return OK;
}

int Schedproc::burst_smooth(unsigned burst)
{
	int i;
	unsigned avg_burst = 0;

	this->burst_history[this->burst_hist_cnt++ % 
BURST_HISTORY_LENGTH] = burst;

	for (i=0; i<BURST_HISTORY_LENGTH; i++)
	{
		if (i >= this->burst_hist_cnt) {
			break;
		}
		avg_burst += this->burst_history[i];
	}	
	avg_burst /= i;

	return avg_burst;
}

extern "C" int do_stop_scheduling(message *m_ptr)
{
	Schedproc *rmp;
	int proc_nr_n;

	/* check who can send you requests */
	if (!accept_message(m_ptr))
		return EPERM;

	if (sched_isokendpt(m_ptr->SCHEDULING_ENDPOINT, &proc_nr_n) != OK) {
		printf("SCHED: WARNING: got an invalid endpoint in OOQ msg "
		"%ld\n", m_ptr->SCHEDULING_ENDPOINT);
		return EBADEPT;
	}

	rmp = &schedproc[proc_nr_n];
#ifdef CONFIG_SMP
	cpu_proc[rmp->cpu]--;
#endif
	rmp->flags = 0; /*&= ~IN_USE;*/

	return OK;
}

int sched_isokendpt(int endpoint, int *proc)
{
	*proc = _ENDPOINT_P(endpoint);
	if (*proc < 0)
		return (EBADEPT); /* Don't schedule tasks */
	if(*proc >= NR_PROCS)
		return (EINVAL);
	if(endpoint != schedproc[*proc].endpoint)
		return (EDEADEPT);
	if(!(schedproc[*proc].flags & IN_USE))
		return (EDEADEPT);
	return (OK);
}

int sched_isemtyendpt(int endpoint, int *proc)
{
	*proc = _ENDPOINT_P(endpoint);
	if (*proc < 0)
		return (EBADEPT); /* Don't schedule tasks */
	if(*proc >= NR_PROCS)
		return (EINVAL);
	if(schedproc[*proc].flags & IN_USE)
		return (EDEADEPT);
	return (OK);
}

extern "C" int do_nice(message *m_ptr)
{
	Schedproc *rmp;
	int rv;
	int proc_nr_n;
	unsigned new_q, old_q, old_max_q;

	/* check who can send you requests */
	if (accept_message(m_ptr))
		return EPERM;

	if (sched_isokendpt(m_ptr->SCHEDULING_ENDPOINT, &proc_nr_n) != OK) {
		printf("SCHED: WARNING: got an invalid endpoint in OOQ msg ""%ld\n", m_ptr->SCHEDULING_ENDPOINT);
		return EBADEPT;
	}

	rmp = &schedproc[proc_nr_n];
	new_q = (unsigned) m_ptr->SCHEDULING_MAXPRIO;
	if (new_q >= NR_SCHED_QUEUES) {
		return EINVAL;
	}

	/* Store old values, in case we need to roll back the changes */
	old_q     = rmp->priority;
	old_max_q = rmp->max_priority;

	/* Update the proc entry and reschedule the process */
	rmp->max_priority = rmp->priority = new_q;

	if ((rv = rmp->schedule_process(SCHEDULE_CHANGE_PRIO | SCHEDULE_CHANGE_QUANTUM)) != OK) 
	{
		rmp->priority     = old_q;
		rmp->max_priority = old_max_q;
	}

	return rv;
}

int Schedproc::schedule_process(unsigned flags)
{
	int err;
	int new_prio, new_quantum, new_cpu;

	this->pick_cpu();

	if (flags & SCHEDULE_CHANGE_PRIO)
		new_prio = this->priority;
	else
		new_prio = -1;

	if (flags & SCHEDULE_CHANGE_QUANTUM)
		new_quantum = this->time_slice;
	else
		new_quantum = -1;

	if (flags & SCHEDULE_CHANGE_CPU)
		new_cpu = this->cpu;
	else
		new_cpu = -1;

	if ((err = call_minix_sys_schedule(this->endpoint, new_prio,
		new_quantum, new_cpu)) != OK) {
		printf("PM: An error occurred when trying to schedule %d: %d\n",this->endpoint, err);
	}

	return err;
}

extern "C" int do_start_scheduling(message *m_ptr)
{
	Schedproc *rmp;
	int rv, proc_nr_n, parent_nr_n;
	
	/* we can handle two kinds of messages here */
	assert(m_ptr->m_type == SCHEDULING_START || 
		m_ptr->m_type == SCHEDULING_INHERIT);

	/* check who can send you requests */
	if (!accept_message(m_ptr))
		return EPERM;

	/* Resolve endpoint to proc slot. */
	if ((rv = sched_isemtyendpt(m_ptr->SCHEDULING_ENDPOINT, &proc_nr_n))
			!= OK) {
		return rv;
	}
	rmp = &schedproc[proc_nr_n];

	/* Populate process slot */
	rmp->endpoint     = m_ptr->SCHEDULING_ENDPOINT;
	rmp->parent       = m_ptr->SCHEDULING_PARENT;
	rmp->max_priority = (unsigned) m_ptr->SCHEDULING_MAXPRIO;
	rmp->burst_hist_cnt = 0;
	if (rmp->max_priority >= NR_SCHED_QUEUES) {
		return EINVAL;
	}

	/* Inherit current priority and time slice from parent. Since there
	 * is currently only one scheduler scheduling the whole system, this
	 * value is local and we assert that the parent endpoint is valid */
	if (rmp->endpoint == rmp->parent) {
		/* We have a special case here for init, which is the first
		   process scheduled, and the parent of itself. */
		rmp->priority   = USER_Q;
		rmp->time_slice = DEFAULT_USER_TIME_SLICE;

		/*
		 * Since kernel never changes the cpu of a process, all are
		 * started on the BSP and the userspace scheduling hasn't
		 * changed that yet either, we can be sure that BSP is the
		 * processor where the processes run now.
		 */
#ifdef CONFIG_SMP
		rmp->cpu = machine.bsp_id;
		/* FIXME set the cpu mask */
#endif
	}
	
	switch (m_ptr->m_type) {

	case SCHEDULING_START:
		/* We have a special case here for system processes, for which
		 * quanum and priority are set explicitly rather than inherited 
		 * from the parent */
		rmp->priority   = rmp->max_priority;
		rmp->time_slice = (unsigned) m_ptr->SCHEDULING_QUANTUM;
		rmp->base_time_slice = rmp->time_slice;
		break;
		
	case SCHEDULING_INHERIT:
		/* Inherit current priority and time slice from parent. Since there
		 * is currently only one scheduler scheduling the whole system, this
		 * value is local and we assert that the parent endpoint is valid */
		if ((rv = sched_isokendpt(m_ptr->SCHEDULING_PARENT,
				&parent_nr_n)) != OK)
			return rv;

		rmp->priority = schedproc[parent_nr_n].priority;
		rmp->time_slice = schedproc[parent_nr_n].time_slice;
		rmp->base_time_slice = rmp->time_slice;
		break;
		
	default: 
		/* not reachable */
		assert(0);
	}

	/* Take over scheduling the process. The kernel reply message populates
	 * the processes current priority and its time slice */
	if ((rv = call_minix_sys_schedctl(0, rmp->endpoint, 0, 0, 0)) != OK) {
		printf("Sched: Error taking over scheduling for %d, kernel said %d\n",
			rmp->endpoint, rv);
		return rv;
	}
	rmp->flags = IN_USE;

	/* Schedule the process, giving it some quantum */
	rmp->pick_cpu();
	while ((rv = rmp->schedule_process(SCHEDULE_CHANGE_ALL)) == EBADCPU) {
		/* don't try this CPU ever again */
		cpu_proc[rmp->cpu] = CPU_DEAD;
		rmp->pick_cpu();
	}

	if (rv != OK) {
		printf("Sched: Error while scheduling process, kernel replied %d\n",
			rv);
		return rv;
	}

	/* Mark ourselves as the new scheduler.
	 * By default, processes are scheduled by the parents scheduler. In case
	 * this scheduler would want to delegate scheduling to another
	 * scheduler, it could do so and then write the endpoint of that
	 * scheduler into SCHEDULING_SCHEDULER
	 */

	m_ptr->SCHEDULING_SCHEDULER = SCHED_PROC_NR;

	return OK;
}

extern "C" int no_sys(int who_e, int call_nr)
{
/* A system call number not implemented by PM has been requested. */
  printf("SCHED: in no_sys, call nr %d from %d\n", call_nr, who_e);
  return(ENOSYS);
}


extern "C" int accept_message(message *m_ptr)
{
	/* accept all messages from PM and RS */
	switch (m_ptr->m_source) {

		case PM_PROC_NR:
		case RS_PROC_NR:
			return 1;
	}
	
	/* no other messages are allowable */
	return 0;
}
