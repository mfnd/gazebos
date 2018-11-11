#include "stm32f4xx.h"  
#include "stdint.h"  
#include "gazebos_conf.h"
#ifndef GAZEBOS_HEADER
#define GAZEBOS_HEADER


#define INITIAL_XPSR    0x01000000

#define T_RUNNING       0
#define T_READY	        1
#define T_WAITING       2

#define NO_FLAG         0x00
#define FLAG_TIMEOUT    0x01

#define NO_WAIT         0x00
#define WAIT_TO         0x01
#define WAIT_SLEEP      0x01
#define WAIT_SIG        0x02
#define WAIT_SEM        0x04
#define WAIT_ENQ        0x08
#define WAIT_DEQ        0x10

#define WAIT_SIG_TO     0x03



#define SVC_SLEEP       0x00
#define SVC_SIGWAIT     0x01
#define SVC_SIGNAL      0x02
#define SVC_SEMWAIT     0x03
#define SVC_SEMSIGNAL   0x04
#define SVC_WAIT_MQ_EMPTY 0x05
#define SVC_WAIT_MQ_FULL 0x06
#define SVC_NOTIFY_MQ    0x07

#define OS_OK           0x00
#define OS_TIMEOUT      0x01

#define WAIT_FOREVER    0xFFFFFFFF



typedef uint32_t os_result;


/*
 *	Type definition for task control block which represents a task, its properties and status.
 *	It is used to control task by OS internals such as the scheduler and resource and delay managers.
 *	
 */
typedef struct TaskControlBlock {
	// Members below is used to track the current status of the task
	uint32_t        *stack_ptr;	// Pointer to the top of the stack which will be used saved from and restored to PSP register during context switches
	uint8_t	        flags;		// Current flag bits of the task. Some of them may be reserved.
	uint8_t	        status;		// Current status of the task (Whether it is running, waiting or ready)
	int8_t	        priority;	// Priority level of the task. Lower priority level indicates a higher priority.
	uint8_t	        wait_type;      // The reason for the task to be waiting. Every bit indicates a different reason. 
	uint32_t        wait_until;	// Timeout time in terms of system ticks. It is used for delays and timeouts if specified.
	uint32_t        wait_signal;    // Used for signaling between tasks. Every bit is a seperate signal. 
                                        // When a task is signalled, the bits in this field corresponding to the set bits in the signal is cleared.
	
        // Pointers below are used for doubly linked lists which is used in queueing mechanisms 
        // such as scheduling, semaphore queues, timeout management etc.
        // The pointers can take NULL values if the task is not currently enqueued or doesn't have any next or previous elements.
        struct TaskControlBlock **wait_queue_address;   // Pointer to the pointer to the head of the wait queue that the task is currently enqueued.
        struct TaskControlBlock *next;                  // Pointer to the next task in the wait queue that the task is currently enqueued.
        struct TaskControlBlock *prev;                  // Pointer to the previous task in the wait queue that the task is currently enqueued.
        struct TaskControlBlock *next_to;               // Pointer to the	next task in the timeout queue
        struct TaskControlBlock *prev_to;               // Pointer to the previous task in the time out queue.

} TaskControlBlock;


typedef struct Semaphore {
	uint32_t capacity;
	TaskControlBlock *wait_queue;
} Semaphore;

#define QUEUE_FULL      0x01
#define QUEUE_EMPTY     0x02

typedef struct MessageQueue {
	uint8_t                 *pool;
    uint16_t                message_size;
    uint16_t                flags;
	uint32_t                capacity;
	uint32_t                write_index;
	uint32_t                read_index;
    Semaphore               sem;
	TaskControlBlock        *enq_wait_queue;
	TaskControlBlock        *deq_wait_queue;
} MessageQueue;

#define MessageQueueDef(QueueName, MessageType, Capacity) \
		MessageType mq_pool_##QueueName[Capacity]; \
		MessageQueue QueueName = { (uint8_t *) mq_pool_##QueueName, sizeof(MessageType), QUEUE_EMPTY ,  Capacity, 0, 0, { 1, 0 }, 0, 0}


os_result mq_dequeue(MessageQueue * mq, uint8_t * deq_data, uint32_t timeout );
os_result mq_enqueue(MessageQueue * mq, uint8_t * enq_data, uint32_t timeout );


os_result semaphore_release(Semaphore *s);
os_result semaphore_acquire(Semaphore *s, uint32_t timeout);
void init_semaphore(Semaphore *sem, uint32_t cap);
uint32_t init_task(void (*fn)(void const* arg), void const *parameter, uint32_t priority);
void gazebos_run(void);

void __svc(SVC_SLEEP) ms_sleep(uint32_t millisecs);
void __svc(SVC_SIGWAIT) wait_signal(uint32_t signal, uint32_t timeout);
void __svc(SVC_SIGNAL) signal_task(uint32_t tid, uint32_t signal);
void __svc(SVC_SEMWAIT) wait_semaphore(Semaphore *sem, uint32_t timeout);
void __svc(SVC_SEMSIGNAL) signal_semaphore(Semaphore *sem);
void __svc(SVC_WAIT_MQ_FULL) gazebos_wait_empty_slot(MessageQueue *mq, uint32_t timeout);
void __svc(SVC_WAIT_MQ_EMPTY)  gazebos_wait_message(MessageQueue *mq, uint32_t timeout);
void __svc(SVC_NOTIFY_MQ)     gazebos_mq_notify(MessageQueue *mq);
#endif
