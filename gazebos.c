
#include "gazebos.h"
#include "gazebos_conf.h"
#include "core_cm4.h" 

#define NULL 0x00000000

volatile uint32_t sys_ticks;

TaskControlBlock tasks[NUM_OF_TASKS];
uint32_t stack_space[NUM_OF_TASKS * TASK_STACK_SIZE];

TaskControlBlock idle_task;
uint32_t idle_stack[IDLE_STACK_SIZE];

TaskControlBlock * ready_queues[NUM_OF_PRIORITY_LVLS+1];
TaskControlBlock * ready_queues_tails[NUM_OF_PRIORITY_LVLS+1];
TaskControlBlock * timeout_queue;
TaskControlBlock * timeout_queue_tail;

volatile TaskControlBlock * curr_task;
volatile TaskControlBlock * next_task;
volatile int32_t curr_tid;
volatile int32_t next_tid;

TaskControlBlock * gazebos_schedule_next(int32_t min_priority);
void gazebos_ready_task(TaskControlBlock *tcb);
void gazebos_enqueue_timeout(TaskControlBlock *tcb);
os_result gazebos_enqueue_resource(TaskControlBlock * tcb, TaskControlBlock ** list);
os_result gazebos_dequeue_resource(TaskControlBlock * tcb);

/*
 *  Idle Daemon.
 *  Run whenever there is not any task ready to run.
 *
 */
void idle_daemon(void const* args) {
	while(1) {
	}
}

/*
 *  Enqueues an element into a message queue.
 *  If there is not any available empty message slot in the queue, calls a system call to wait for an available message slot.
 *  If there was a task already waiting for a message, calls a system call to ready that task.
 *  (Public API)
 *
 *  @param mq Pointer to the message queue
 *  @param deq_data Pointer to the memory area into which the dequeued message will be copied.
 *  @param timeout Timeout amount in terms of system ticks
 */
os_result mq_enqueue(MessageQueue * mq, uint8_t * enq_data, uint32_t timeout ) {
    os_result   res;
    uint32_t    start_ticks = sys_ticks;
    while(1) {
        res = semaphore_acquire(&mq->sem, timeout);
        if (res == OS_TIMEOUT) {
            return OS_TIMEOUT;
        }
        if (mq->flags & QUEUE_FULL) {
            gazebos_wait_empty_slot(mq, timeout - (sys_ticks - start_ticks));
            if( curr_task->flags & FLAG_TIMEOUT) {
                return OS_TIMEOUT;
            }
        }
        else {
            break;
        }
    }
    memcpy(mq->pool + mq->write_index * mq->message_size, enq_data, mq->message_size);
    mq->write_index = (mq->write_index >= mq->capacity - 1) ? 0 
                                                            : mq->write_index + 1;
    if (mq->write_index == mq->read_index) {
        mq->flags |= QUEUE_FULL;
    }
    mq->flags &= ~QUEUE_EMPTY;
    if (mq->deq_wait_queue) { 
        gazebos_mq_notify(mq);
    }
    else {
        semaphore_release(&mq->sem);
    }
    return OS_OK;
}

/*
 *  Dequeues an element from a message queue.
 *  If there is not any available message in the queue, calls a system call to wait for a message.
 *  If there was a task already waiting for a empty slot in the queue, calls a system call to ready that task.
 *  (Public API)
 *
 *  @param mq Pointer to the message queue
 *  @param deq_data Pointer to the memory area into which the dequeued message will be copied.
 *  @param timeout Timeout amount in terms of system ticks
 */
os_result mq_dequeue(MessageQueue * mq, uint8_t * deq_data, uint32_t timeout ) {
    os_result   res;
    uint32_t    start_ticks = sys_ticks;
    
    while(1) {
        res = semaphore_acquire(&mq->sem, timeout);
        if (res == OS_TIMEOUT) {
            return OS_TIMEOUT;
        }
        if (mq->flags & QUEUE_EMPTY) {
            gazebos_wait_message(mq, timeout - (sys_ticks - start_ticks));
            if( curr_task->flags & FLAG_TIMEOUT) {
                return OS_TIMEOUT;
            }
        }
        else {
            break;
        }
    }
    memcpy(deq_data, mq->pool + mq->read_index * mq->message_size, mq->message_size);
    mq->read_index = (mq->read_index >= mq->capacity - 1) ? 0 
                                                          : mq->read_index + 1;
    if (mq->write_index == mq->read_index) {
        mq->flags |= QUEUE_EMPTY;
    }
    
    mq->flags &= ~QUEUE_FULL;
    if (mq->enq_wait_queue) {
        gazebos_mq_notify(mq);
    }
    else {
        semaphore_release(&mq->sem);
    }
    return OS_OK;
}


/*
 *  Acquires a semamphore token.
 *  If there is not any available semaphore tokens. It calls a system call to wait for the semaphore.
 *  (Public API)
 *
 *  @param s Address of the semaphore structure
 *  @param timeout Timeout amount in terms of system ticks
 */
os_result semaphore_acquire(Semaphore *s, uint32_t timeout) {
	uint32_t capacity;
	
	while(1) {
		capacity = __ldrex(&s->capacity);
		if(capacity > 0) {
			if(!__strex(capacity-1, &s->capacity)) {
				break;
			}
		}
		else {
			if( timeout > 0 ) {
				wait_semaphore(s, timeout);
				if(curr_task->flags & FLAG_TIMEOUT) {
					return OS_TIMEOUT;
				}
			}
			else return OS_TIMEOUT;
		}
	}
	return OS_OK;
}
	
/*
 *  Releases a semamphore token.
 *  If its capacity was zero before release, it calls a system call to signal a task which was waiting for it.
 *  (Public API)
 *
 *  @param s Address of the semaphore structure
 */
os_result semaphore_release(Semaphore *s) {
	uint32_t capacity;
	
	while(1) {
		capacity = __ldrex(&s->capacity);
		if(!__strex(capacity+1, &s->capacity)) {
			break;
		}
	}
	if(capacity == 0) {
		signal_semaphore(s);
	}
	return OS_OK;
}

/*
 *  Runs the first task and switches to thread mode.
 *  This should be only called by gazebos_run function and should be called only once.
 *  (Internal API) 
 * 
 *  @param curr_stack Stack pointer of the task that will be run
 *  @param fn Function pointer of the task that will be run
 */
__asm void gazebos_switch_to_thread_mode(uint32_t curr_stack, uint32_t fn) {
		msr psp, r0
		mov r0, #3
		msr control, r0
		push {r1}
		pop {pc}
}

/*
 * ISR for SysTick.
 * Increments system tick counts and handles any timeout events and makes the corresponding tasks ready.
 * 
 */
void SysTick_Handler(void) 
{
	TaskControlBlock *waiting_tcb;

	// Increment system tick count
	sys_ticks++;

	// Check whether there is task waiting for a timeout and whether it is timed out or not. 
	// If there is, remove them from wait queues of the resource they are waiting and add them to ready queue.
	while(timeout_queue != NULL && timeout_queue->wait_type & WAIT_TO && timeout_queue->wait_until <= sys_ticks) {
		waiting_tcb = timeout_queue;
		waiting_tcb->flags |= FLAG_TIMEOUT;
		gazebos_dequeue_resource(waiting_tcb);
		gazebos_ready_task(waiting_tcb);
	}
	
	// Schedule a new task up to the priority of the current task. 
	// If a new task is selected, add the current task to ready queue and enable context switch.
	if(curr_task != gazebos_schedule_next(curr_task->priority)) {
		gazebos_ready_task(curr_task);
		SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
	}
}


/*
 *  SVC_Handler
 *  Handles system calls by threads
 */
void SVC_Handler_C(unsigned int * args) 
{
	uint32_t svc_id = ((char*)args[6])[-2];
	TaskControlBlock *tcb, *tcb_prev;
	Semaphore	*sem;
    MessageQueue *mq;
    curr_task->flags &= ~FLAG_TIMEOUT;
	switch(svc_id) {
		case SVC_SLEEP:
			curr_task->wait_until = sys_ticks + args[0];
			curr_task->wait_type   = WAIT_SLEEP;
			curr_task->flags			= NO_FLAG;
			gazebos_enqueue_timeout(curr_task);
			gazebos_schedule_next(NUM_OF_PRIORITY_LVLS);
			SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
			return;
		case SVC_SIGWAIT:
			curr_task->wait_signal = args[0];
			curr_task->flags			= NO_FLAG;
			// TODO: Maybe add another condition for checking whether there is a timeout or not.
			if(args[1] > 0 && args[1] < WAIT_FOREVER) {
				curr_task->wait_until  = sys_ticks +args[1];
				curr_task->wait_type   = WAIT_SIG_TO;
				gazebos_enqueue_timeout(curr_task);
			} 
			else {
				curr_task->wait_type	 = WAIT_SIG;
			}
			curr_task->status 	 = T_WAITING;
			gazebos_schedule_next(NUM_OF_PRIORITY_LVLS);
			SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
			return;
		case SVC_SIGNAL:
			tcb = &tasks[args[0]];
			tcb->wait_signal &= ~args[1];
			if(tcb->wait_signal == NULL && tcb->wait_type & WAIT_SIG) {
				gazebos_ready_task(tcb);
				if(curr_task != gazebos_schedule_next(curr_task->priority - 1)) {
					gazebos_ready_task(curr_task);
					SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
				}
			}
			return;
		case SVC_SEMWAIT:
			curr_task->flags			= NO_FLAG;
			sem = (Semaphore *) args[0];
			if(sem->capacity == 0) {
				curr_task->wait_type = WAIT_SEM;
				if(args[1] > 0 && args[1] < WAIT_FOREVER) {
						curr_task->wait_until  = sys_ticks +args[1];
						curr_task->wait_type   |= WAIT_TO;
						gazebos_enqueue_timeout(curr_task);
				}
				gazebos_enqueue_resource(curr_task, &sem->wait_queue);
				curr_task->status = T_WAITING;
				gazebos_schedule_next(NUM_OF_PRIORITY_LVLS);
				SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
			}
			else {
				sem->capacity--;
			}
			return;
		case SVC_SEMSIGNAL:
			sem = (Semaphore *) args[0];
			if(sem->capacity > 0 && sem->wait_queue) {
                
				tcb = sem->wait_queue;
				sem->wait_queue = tcb->next;
				if(tcb->next) {
					tcb->next->prev = NULL;
				}
				
				
				gazebos_ready_task(tcb);
				if(curr_task != gazebos_schedule_next(curr_task->priority - 1)) {
					gazebos_ready_task(curr_task);
					SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
				}
			}
			return;
        case SVC_WAIT_MQ_EMPTY:
            mq = (MessageQueue *) args[0];
            if(args[1] > 0 && args[1] < WAIT_FOREVER) {
				curr_task->wait_until  = sys_ticks +args[1];
				curr_task->wait_type   |= WAIT_TO;
				gazebos_enqueue_timeout(curr_task);
			}
            mq->sem.capacity = 1;
			gazebos_enqueue_resource(curr_task, &mq->deq_wait_queue);
			curr_task->status = T_WAITING;
			gazebos_schedule_next(NUM_OF_PRIORITY_LVLS);
			SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
            return;
        case SVC_WAIT_MQ_FULL:
            mq = (MessageQueue *) args[0];
            if(args[1] > 0 && args[1] < WAIT_FOREVER) {
				curr_task->wait_until  = sys_ticks +args[1];
				curr_task->wait_type   |= WAIT_TO;
				gazebos_enqueue_timeout(curr_task);
			}
            mq->sem.capacity = 1;
			gazebos_enqueue_resource(curr_task, &mq->enq_wait_queue);
			curr_task->status = T_WAITING;
			gazebos_schedule_next(NUM_OF_PRIORITY_LVLS);
			SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
            return;
        case SVC_NOTIFY_MQ:
            mq = (MessageQueue *) args[0];
            if ( !(mq->flags & QUEUE_FULL) && mq->enq_wait_queue) {
                tcb = mq->enq_wait_queue;
				mq->enq_wait_queue = tcb->next;
				if(tcb->next) {
					tcb->next->prev = NULL;
				}
				
				gazebos_ready_task(tcb);
            }
            if ( !(mq->flags & QUEUE_EMPTY) && mq->deq_wait_queue) {
                tcb = mq->deq_wait_queue;
				mq->deq_wait_queue = tcb->next;
				if(tcb->next) {
					tcb->next->prev = NULL;
				}
				gazebos_ready_task(tcb);
            }
            mq->sem.capacity = 1;
            if(curr_task != gazebos_schedule_next(curr_task->priority - 1)) {
				gazebos_ready_task(curr_task);
				SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
			}
	}
	
	
}

__asm void SVC_Handler(void) {
	extern SVC_Handler_C
	
	MRS	r0, psp
	B	SVC_Handler_C
	
}

/*
 *  PendSV_Handler
 *  PendSV exception is used for context switching.
 *  When the PendSV bit is set, OS performs a context switch after all interrupts are serviced.
 */
__asm void PendSV_Handler(void) {
		extern curr_task
		extern next_task
		PRESERVE8
		
		PUSH {LR}
		MRS r0, psp   
    STMFD r0!, {r4-r11}
    LDR r1, =curr_task
		LDR r2, [r1]
		STR r0, [r2]
		//STR r0, [r1]
		
		LDR r2, =next_task
		LDR r3, [r2]
		LDR r0, [r3]
		LDMFD r0!, {r4-r11}   
    MSR psp, r0
		STR r3, [r1]
    POP {LR}
		BX LR
}

/*
 *  Selects next task to be dispatched. PendSV exception must be set in order to switch tasks.
 *  (Internal API)
 *
 *  @param min_priority Minimum priority level for selecting a task
 */
TaskControlBlock* gazebos_schedule_next(int32_t min_priority) {
	int32_t i;
	int32_t prev_tid = -1;

	for(i = 0; i <= min_priority; i++) {
		if(ready_queues[i]) {
			next_task = ready_queues[i];
			ready_queues[i] = next_task->next;
			if(ready_queues[i]) {
				ready_queues[i]->prev = NULL;
			}
			else {
				ready_queues_tails[i] = NULL;
			}
			next_task->next = NULL;
			break;
		}
	}
	return next_task;
}

void init_semaphore(Semaphore *sem, uint32_t cap) {
	sem->capacity = cap;
	sem->wait_queue = NULL;
}

uint32_t init_task( void (*fn)(void const* arg), void const *parameter, uint32_t priority ) {
	int i;
	int32_t stack_top;
	for(i = 0; i < NUM_OF_TASKS; i++) {
		if(tasks[i].stack_ptr == 0) {
			stack_top = TASK_STACK_SIZE * (i+1);
			tasks[i].stack_ptr = &stack_space[stack_top - 16];
			stack_space[stack_top-1] = INITIAL_XPSR;
			stack_space[stack_top-2] = (uint32_t) fn;
			stack_space[stack_top-3] = 0;
			stack_space[stack_top-8] = (uint32_t) parameter;
			tasks[i].priority = priority;
			tasks[i].next = NULL;
			tasks[i].prev	= NULL;
			tasks[i].next_to = NULL;
			tasks[i].prev_to = NULL;
			tasks[i].wait_queue_address = NULL;
			gazebos_ready_task(&tasks[i]);
			break;
		}
	}
	return i;
}

/*
 *  Starts the operating system by scheduling and dispatching first task
 *  Should be called after all necessarry structures are created and initialized.
 *
 */
void gazebos_run() {
	int i;
	uint32_t curr_stack;
	
	idle_task.stack_ptr = &idle_stack[IDLE_STACK_SIZE-16];
	idle_stack[IDLE_STACK_SIZE-1] = INITIAL_XPSR;
	idle_stack[IDLE_STACK_SIZE-2] = (uint32_t) idle_daemon;
	idle_stack[IDLE_STACK_SIZE-3] = 0;
	idle_stack[IDLE_STACK_SIZE-8] = 0;
	idle_task.priority						= NUM_OF_PRIORITY_LVLS;
	idle_task.next								= NULL;
	idle_task.prev	= NULL;
	idle_task.next_to = NULL;
	idle_task.prev_to = NULL;
	idle_task.wait_queue_address = NULL;
	gazebos_ready_task(&idle_task);
	
	//gazebos_init_task(idle_daemon, 0, 2);
	

	curr_task = gazebos_schedule_next(NUM_OF_PRIORITY_LVLS);
	
	curr_task->stack_ptr = curr_task->stack_ptr + 16;

	gazebos_switch_to_thread_mode((uint32_t) curr_task->stack_ptr, (uint32_t) *(curr_task->stack_ptr - 2));
	
}

/*
 *  Ready a task and remove any timeouts of it from the timeout queue.
 *  (Internal API)
 *
 */
void gazebos_ready_task(TaskControlBlock *tcb) {
	TaskControlBlock	*	iter, * iter_prev ;
	uint8_t task_priority = tcb->priority;		
	tcb->next = NULL;
	tcb->prev	= NULL;
	
	if(tcb->wait_type & WAIT_TO) {
		if(tcb->prev_to == NULL) {
			timeout_queue = tcb->next_to;
			if(tcb->next_to != NULL) {
				tcb->next_to->prev_to = NULL;
			}
		}
		else {
			tcb->prev_to->next_to = tcb->next_to;
			if(tcb->next_to != NULL) {
				tcb->next_to->prev_to = tcb->prev_to;
			}
		}
		tcb->prev_to = NULL;
		tcb->next_to = NULL;
		tcb->wait_until = 0;
		tcb->wait_type = NO_WAIT;
	}
	
	if(!ready_queues[task_priority]) {
		ready_queues[task_priority] = tcb;
		ready_queues_tails[task_priority] = tcb;
	}
	else {
		ready_queues_tails[task_priority]->next = tcb;
		tcb->prev	= ready_queues_tails[task_priority];
		ready_queues_tails[task_priority]			 = tcb;	
	}
	tcb->status = T_READY;
	tcb->wait_type = NO_WAIT;
}

/*
 *  Adds a task to timeout queue based on its wait_until value
 *  (Internal API)
 *
 */
void gazebos_enqueue_timeout(TaskControlBlock *tcb) {
	TaskControlBlock *iter_prev;
	TaskControlBlock *iter;
	tcb->status = T_WAITING;
	tcb->prev_to 	= NULL;
	tcb->next_to		= NULL;
	if(timeout_queue == NULL) {
		timeout_queue = tcb;
		//timeout_queue_tail = tcb;
	}
	else {
		iter_prev = timeout_queue;
		if(iter_prev->wait_until > tcb->wait_until) {
			timeout_queue = tcb;
			tcb->next_to = iter_prev;
			iter_prev->prev_to = tcb;
		}
		else {
			iter = iter_prev->next_to;
			while(iter != NULL) {
				if(iter->wait_until > tcb->wait_until) {
					break;
				}
				iter_prev = iter;
				iter 			= iter->next_to;
			}
			tcb->next_to = iter;
			if(iter) iter->prev_to = tcb;
			iter_prev->next_to = tcb;
			tcb->prev_to = iter_prev;
		}
	}
}

/*
 *  Adds a task to a resource wait queue based on task priority.
 *  (Internal API)
 *
 */
os_result gazebos_enqueue_resource(TaskControlBlock * tcb, TaskControlBlock ** list) {
	TaskControlBlock * iter, *iter_prev;
	iter_prev = *list;
	tcb->wait_queue_address = list;
	if(iter_prev == NULL) {
		*list =  tcb;
		tcb->prev = NULL;
		tcb->next	= NULL;
	} 
	else {
		if(iter_prev->priority > tcb->priority) {
			*list = tcb;
			tcb->prev  = NULL;
			tcb->next  = iter_prev;
			iter_prev->prev = tcb; 
		}
		else {
			iter = iter_prev->next;
			while(iter) {
				if(iter->priority > tcb->priority) {
					break;
				}
				iter_prev = iter;
				iter			= iter->next;
			}
			iter_prev->next = tcb;
			tcb->prev				= iter_prev;
			if(iter) {
				tcb->next = iter;
				iter->prev = tcb;
			}
			else {
				tcb->next = NULL;
			}
		}
		
	}
	return OS_OK;
}

/*
 *  Removes a task from a resource wait queue.
 *  (Internal API)
 *
 */
os_result gazebos_dequeue_resource(TaskControlBlock * tcb) {
	TaskControlBlock * list_head;
	if(tcb->wait_queue_address) {
		list_head = *(tcb->wait_queue_address);
		if(list_head == tcb) {
			
			*(tcb->wait_queue_address) = tcb->next;
			if(tcb->next) {
				tcb->next->prev = NULL;
			}
			
		}	else {
			tcb->prev = tcb->next;
			if(tcb->next) {
				tcb->next->prev = tcb->prev;
			}
		}
	}
	tcb->next = NULL;
	tcb->prev = NULL;
	tcb->wait_queue_address = NULL;
	return OS_OK;
}


