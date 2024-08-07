/*
   This driver supports the Unicorn ADSL chipset from STMicroelectronics.
   The chipset consists of the ADSL DMT transceiver ST70138 and the
   ST70174 Analog Front End (AFE).
   This file contain the rAPI(reduced API) functions.
   rAPI is the interface between the Modem SW and the Operating System (here Linux).
 */
/*
  Updated to work with Linux kernel >= 3.6.10 by
  Zbigniew Luszpinski 2013-05-04 <zbiggy(a)o2,pl>
*/
#include <linux/version.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 33)
#include <linux/autoconf.h>
#else
#include <generated/autoconf.h>
#endif
#include <linux/version.h>

#if defined(CONFIG_MODVERSIONS) && (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0))
#include <linux/modversions.h>
#endif

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/list.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 4, 0)
#include <asm/system.h>
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,15,0)
#include <linux/timex.h>
#else
#include <asm/timex.h>
#endif
#include <linux/timer.h>
#include <linux/slab.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)
#include <linux/sched/signal.h>
#else
#include <linux/sched.h>
#endif
#include <linux/time.h>
#include <linux/delay.h>
#include <linux/signal.h>
//#include <linux/smp_lock.h>
#include <linux/kthread.h>
#include "../include/types.h"
#include "../include/hal.h"
#include "../include/hard.h"
#include "../include/amsw_intf_types.h"
#include "../include/amsw_ant.h"
#include "../include/amas.h"
#include "../include/tracetool.h"
#include "../include/rapi.h"
#include "../include/debug.h"

#if  (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,27))
#include <linux/semaphore.h>
#else
#include <asm/semaphore.h>
#endif

//======================================================================
// C++ support
// This is needed when using C++ in linux modules
//======================================================================

// All static constructors/destructors in modem_ant_USB.o

extern void _GLOBAL__I_aocDbg(void);
extern void _GLOBAL__I_eocDbg(void);
extern void _GLOBAL__I_hsDbg(void);
extern void _GLOBAL__I_modemSubsystemInfo(void);
extern void _GLOBAL__I_channelConfigurationUpstreamPOTS(void);
extern void _GLOBAL__I_interfmessage(void);

//======================================================================
// Trace  support
// The MSW will call the PRINT_xxx functions to trace.
//======================================================================

extern unsigned long MswDebugLevel;


//======================================================================
//	MSW INTERRUPT MANAGEMENT
//======================================================================

DWORD tosca_hardITABLE[14] = {0,0,0,0,0,0,0,0,0,0,0,0,0}; // TOSCA Interrupt table visible to MSW
WORD tosca_softITABLE[28] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};         

static DWORD tosca_intr_set = 0; // != 0 if interrupt set
static struct task_struct *tosca_intr_thread_id = 0;
static BOOLEAN tosca_intr_cancelled = FALSE; // TRUE if the INTR thread loop is cancelled
static BOOLEAN tosca_intr_disabled = FALSE; // TRUE if the INTR thread does nothing
DECLARE_WAIT_QUEUE_HEAD(tosca_intr_wait); // Wait queue for TOSCA Interrupt mgt



//======================================================================
// rAPI support
// The reduced API used by the MSW
//======================================================================

#define MAX_MEM 1000000UL

#define RAPI_SIGNATURE 0xABC0UL
#define MEM_TYPE (RAPI_SIGNATURE + 0x00)
#define TASK_TYPE (RAPI_SIGNATURE + 0x01)
#define SEM_TYPE (RAPI_SIGNATURE + 0x02)
#define Q_TYPE (RAPI_SIGNATURE + 0x03)
#define MSG_TYPE (RAPI_SIGNATURE + 0x04)
#define TIMER_TYPE (RAPI_SIGNATURE + 0x05)

#define HEAP_HDR \
	struct list_head list; \
unsigned long type;\
unsigned long size;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0))
	struct legacy_timer_emu {
		struct timer_list t;
		void (*function)(unsigned long);
		unsigned long data;
	};
#endif

struct rapi_heap {
	HEAP_HDR
};

struct rapi_mem {
	HEAP_HDR
		char mem[1];
};

struct rapi_task {
	HEAP_HDR
		char name[4];
	DWORD priority;
	DWORD args[4];
	START_FUNC start_addr;
	struct task_struct *thread;
};

struct rapi_sem {
	HEAP_HDR
		char name[4];
	struct semaphore sem;
#if defined (LINUX_VERSION_CODE) && (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0))
    struct legacy_timer_emu timer;
#else
	struct timer_list timer;
#endif
};

struct rapi_q {
	HEAP_HDR
		char name[4];
	spinlock_t msg_q_lock;
	struct list_head msg_q;
	DWORD q_sem;
};

struct rapi_msg {
	HEAP_HDR
		DWORD msg_buf[4];
	DWORD timer_id;
};

struct rapi_timer {
	HEAP_HDR
#if defined (LINUX_VERSION_CODE) && (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0))
        struct legacy_timer_emu timer;
#else
	    struct timer_list timer;
#endif
	DWORD qid;
	DWORD mode;
	DWORD interval;
	DWORD userdata;
	struct task_struct * owner;
};

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0))
//======================================================================
// Timer compatibility code
//======================================================================
static void legacy_timer_emu_func(struct timer_list *t)
{
	struct legacy_timer_emu *lt = from_timer(lt, t, t);
	lt->function(lt->data);
}
#endif

//======================================================================
// Memory allocation variables.
//======================================================================
static struct rapi_heap *min_addr=(struct rapi_heap *)-1,*max_addr=(struct rapi_heap *)0L;
static unsigned long tot_mem=0L,max_mem=0L;
static unsigned long obj_counters[6] = {0L};

static LIST_HEAD(rapi_heap_list);
//static spinlock_t rapi_heap_lock=SPIN_LOCK_UNLOCKED;
static DEFINE_SPINLOCK(rapi_heap_lock);

static atomic_t running_tasks = {0};

static struct semaphore rapi_thread_lock; // To serialize rAPI threads

//static spinlock_t rapi_timer_lock=SPIN_LOCK_UNLOCKED;
static DEFINE_SPINLOCK(rapi_timer_lock);
//static spinlock_t tosca_lock=SPIN_LOCK_UNLOCKED;
static DEFINE_SPINLOCK(tosca_lock);

//======================================================================
// C++ support
// This is needed when using C++ in linux modules
//======================================================================

// call all the constructors

void __do_global_ctors (void)
{
	_GLOBAL__I_aocDbg();
	_GLOBAL__I_eocDbg();
	_GLOBAL__I_hsDbg();
	_GLOBAL__I_modemSubsystemInfo();
	_GLOBAL__I_channelConfigurationUpstreamPOTS();
	_GLOBAL__I_interfmessage();
}

// call all the destructors
void __do_global_dtors (void)
{
	//_GLOBAL__D_Vendor_Id_code_ECI();
	//_GLOBAL__D_aocDbg();
	//_GLOBAL__D_eocDbg();
	//_GLOBAL__D_hsDbg();
	//_GLOBAL__D_modemSubsystemInfo();
	//_GLOBAL__D_prs();
}

// C++ stubs
extern void __gxx_personality_v0(void)
{
	DBG(RAPI_D,"__gxx_personality_v0 called\n");
}

	extern void
__cxa_pure_virtual(void)
{
	DBG(RAPI_D,"__cxa_pure_virtual called\n");
}

#if __GNUC__ >= 3
	extern void
__builtin_delete(void *ptr)
{
	DBG(RAPI_D,"ptr=%p\n",ptr);
	xm_retmem(ptr);
}

	extern void
__builtin_vec_delete(void *ptr)
{
	DBG(RAPI_D,"ptr=%p\n",ptr);
	xm_retmem(ptr);
}

	extern void *
__builtin_vec_new(size_t size)
{
	void *ptr;
	xm_getmem(size,&ptr);
	DBG(RAPI_D,"size=%d,ptr=%p\n",size,ptr);
	return ptr;
}

	extern void
__pure_virtual(void)
{
	DBG(RAPI_D,"__pure_virtual called\n");
}

#else

	extern void
_ZdlPv(void *ptr)
{
	// operator delete(void*)
	xm_retmem(ptr);
}

	extern void
_ZdaPv(void *ptr)
{
	// operator delete[](void*)
	xm_retmem(ptr);
}

	extern void *
_Znaj(unsigned size)
{
	// operator new[](unsigned)
	void *ptr;

	if (xm_getmem(size,&ptr) == SUCCESS) {
		//PRINT_INFO("new[]: size=%d,ptr=%p\n",size,ptr);
	} else {
		ptr = NULL;
		PRINT_ERROR("### xm_getmem failed,size=%ld\n",size);
	}
	return ptr;
}
#endif


//======================================================================
// Trace  support
// The msw will call the PRINT_xxx functions to trace.
//======================================================================
#if DEBUG
extern unsigned long timer_int_counter;

static DWORD start_time=0;

static DWORD get_timestamp(void)
{
#ifdef USE_HW_TIMER
	return timer_int_counter<<1;
#else
	return xtm_elapse(start_time);
#endif
}
extern int PRINT_INFO(const char *format, ...)
{
	va_list args;
	int i;
	char buf[256];
	char *p = buf;

	if (MswDebugLevel > 0) return  0;

	va_start(args, format);
	i = vsprintf(buf, format, args);
	va_end(args);
	if (i > sizeof(buf)) BUG();
	if (p[i-2] == '\r') p[i-2] = '\n';
	if (p[i-1] == '\r') p[i-1] = '\n';

	return printk(KERN_INFO "unicorn_msw-%ld: " "%s", get_timestamp(),p);
}

extern int PRINT_WARNING(const char *format, ...)
{
	va_list args;
	int i;
	char buf[256];
	char *p = buf;

	if (MswDebugLevel > 1) return  0;


	va_start(args, format);
	i = vsprintf(buf, format, args);
	va_end(args);
	if (i > sizeof(buf)) BUG();
	if (p[i-2] == '\r') p[i-2] = '\n';
	if (p[i-1] == '\r') p[i-1] = '\n';

	return printk(KERN_WARNING "unicorn_msw: " "%s",p);
}

extern int PRINT_ERROR(const char *format, ...)
{
	va_list args;
	int i;
	char buf[256];
	char *p = buf;

	va_start(args, format);
	i = vsprintf(buf, format, args);
	va_end(args);
	if (i > sizeof(buf)) BUG();
	if (p[i-2] == '\r') p[i-2] = '\n';
	if (p[i-1] == '\r') p[i-1] = '\n';

	return printk(KERN_ERR "unicorn_msw: " "%s",p);
}
#endif

//======================================================================
//	MSW INTERRUPT MANAGEMENT
//======================================================================

//----------------------------------------------------------------------
//	Copy the TOSCA software interrupt table from the hardware it. table.
//	Mask all bits according to the interrupt mask table.
//	Clear all bits of the hard table that have been set in the soft table.
//----------------------------------------------------------------------
static WORD CopySoftIntrTable(void)
{
	WORD sum; 
	UINT i;
	for (i=0,sum=0; i<14; i++) {
		tosca_softITABLE[i] = tosca_hardITABLE[i] & tosca_softITABLE[i+14];
		
#if  (LINUX_VERSION_CODE >= KERNEL_VERSION(4,3,0))
        atomic_and(~tosca_softITABLE[i],(atomic_t *)(&tosca_hardITABLE[i]));
#else
        atomic_clear_mask(tosca_softITABLE[i],&tosca_hardITABLE[i]);
#endif
		//tosca_hardITABLE[i] &= ~tosca_softITABLE[i];
		sum += tosca_softITABLE[i];
	}
	return sum;
}

ST_STATUS setBitIrqMaskTable(BYTE regIndex, BYTE irqNr)
{
	tosca_softITABLE[regIndex+14] |= (1<<irqNr);
	return SUCCESS;
}
ST_STATUS clearBitIrqMaskTable(BYTE regIndex, BYTE irqNr)
{
	tosca_softITABLE[regIndex+14] &= ~(1<<irqNr);
	return SUCCESS;
}
ST_STATUS setIrqMaskTableEntry(BYTE regIndex, WORD mask)
{
	tosca_softITABLE[regIndex+14] = mask;
	return SUCCESS;
}
ST_STATUS getIrqTableEntry(BYTE regIndex, WORD* tableEntry)
{
	*tableEntry = tosca_softITABLE[regIndex];
	return SUCCESS;
}

//----------------------------------------------------------------------
//  IntrHandler
//----------------------------------------------------------------------
static ST_STATUS tosca_intr_handler(void)
{
	unsigned long flags;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,15,0)
    bool wait_condition_satisfied = false; 
#endif
	for(;;) {
		DBG(TOSCA_D,"tosca_intr_cancelled=%d,tosca_intr.intr_disabled=%d\n",
				tosca_intr_cancelled,tosca_intr_disabled);

		// Test if the interrupt loop has been cancelled
		if (tosca_intr_cancelled) {
			DBG(1,"tosca_intr_cancelled\n");
			tosca_intr_cancelled = FALSE;
			return FAILURE;
		}


		spin_lock_irqsave(&tosca_lock, flags);

		// If the Interrupt is enabled
		if (!tosca_intr_disabled) {
			WORD set;
			// -----------------------------------------------------------
			// Copies the Software Interrupt table applying the mask table
			// while clearing the corresponding bits in the hardware table
			// this must be done synchronized with the ISR
			// -----------------------------------------------------------
			set = CopySoftIntrTable();

			if (set) {
				if (tosca_intr_disabled) {
					DBG(TOSCA_D,"tosca interrupt lost\n");
				}

				spin_unlock_irqrestore(&tosca_lock, flags);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,15,0)
                wait_condition_satisfied = true; 
#endif
				break;	// some bit is set in the interrupt table
			}
		}

		spin_unlock_irqrestore(&tosca_lock, flags);

		rapi_unlock();
		// Waits for the TOSCA interrupt event generated by the ISR
		
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,15,0)
        wait_event_interruptible_timeout(tosca_intr_wait, wait_condition_satisfied, (TOSCA_INTR_WDOG*HZ)/1000);
#else
        interruptible_sleep_on_timeout(&tosca_intr_wait,(TOSCA_INTR_WDOG*HZ)/1000);
#endif
		rapi_lock();
	}
	return SUCCESS;
}

//----------------------------------------------------------------------
// wake up the tosca interrupt task
//----------------------------------------------------------------------
extern void tosca_interrupt(void)
{
	wake_up(&tosca_intr_wait);
}

//----------------------------------------------------------------------
//	TOSCA Interrupt Task
//----------------------------------------------------------------------
static void tosca_interrupt_task(DWORD arg1,DWORD arg2,DWORD arg3,DWORD arg4)
{
	ITHANDLER Handler = (ITHANDLER)arg1;
	DWORD DevNumber = arg3;
	ST_STATUS  status;

	tosca_intr_thread_id = current;

	for(;;) {
		status = tosca_intr_handler();
		if (status != SUCCESS) break;
		DBG(TOSCA_D,"Entering MSW interrupt handler\n");
		Handler(DevNumber);
	}
	tosca_intr_thread_id = 0;
}

//----------------------------------------------------------------------
//	USB_checkIntContext
//----------------------------------------------------------------------
BOOL USB_checkIntContext(void)
{

	return tosca_intr_thread_id == current;
}

//----------------------------------------------------------------------
//	board_set_intr_handler
//----------------------------------------------------------------------
DWORD board_set_intr_handler(DWORD intr,ITHANDLER handler,DWORD devNumber)
{
	DWORD tid;
	DWORD s;
	DWORD targs[4];

	DBG(TOSCA_D,"intr=%ld\n",intr);

	if (tosca_intr_set) {
		DBG(RAPI_D,"failed (invalid argument)\n");
		return ERR_BSP_ILL_INTR;
	}
	if (handler == NULL) {
		DBG(RAPI_D,"failed (invalid argument)\n");
		return ERR_BSP_ILL_INTR;
	}
	tosca_intr_set = intr;

	s = xt_create("INTR",255,0,0,0,&tid);
	if (s == FAILURE) {
		DBG(RAPI_D,"xt_create() failed for Interrupt Task\n");
		return ERR_BSP_ILL_INTR;
	}

	memset(targs,0,sizeof(targs));
	targs[0] = (DWORD)handler;
	targs[1] = intr;
	targs[2] = devNumber;

	s = xt_start(tid,0,tosca_interrupt_task,targs);
	if (s == FAILURE) {
		DBG(RAPI_D,"ERROR: xt_start() failed for Interrupt Task\n");
		return ERR_BSP_ILL_INTR;
	}
	return SUCCESS;
}

//----------------------------------------------------------------------
//	board_reset_intr_handler
//----------------------------------------------------------------------
DWORD board_reset_intr_handler(DWORD intr)
{
	DBG(TOSCA_D,"intr=%ld\n",intr);

	if (intr != tosca_intr_set) {
		DBG(RAPI_D,"failed (invalid argument)");
		return ERR_BSP_ILL_INTR;
	}
	tosca_intr_set = 0L;
	tosca_intr_cancelled = TRUE;
	wake_up(&tosca_intr_wait);

	return SUCCESS;
}

//----------------------------------------------------------------------
//	board_clear_intr_pending
//----------------------------------------------------------------------
DWORD board_clear_intr_pending(DWORD intr)
{
	if (intr != tosca_intr_set) {
		DBG(RAPI_D,"failed (invalid argument %ld)\n",intr);
		return ERR_BSP_ILL_INTR;
	}
	return SUCCESS;
}

//----------------------------------------------------------------------
//	board_mask_intr
//----------------------------------------------------------------------
DWORD board_mask_intr(DWORD intr)
{
	if (intr != tosca_intr_set) {
		DBG(RAPI_D,"failed (invalid argument %ld)\n",intr);
		return ERR_BSP_ILL_INTR;
	}
	board_disable_intrs();
	return SUCCESS;
}

//----------------------------------------------------------------------
//	board_unmask_intr
//----------------------------------------------------------------------
DWORD board_unmask_intr(DWORD intr)
{
	if (intr != tosca_intr_set) {
		DBG(RAPI_D,"failed (invalid argument %ld)\n",intr);
		return ERR_BSP_ILL_INTR;
	}

	board_enable_intrs();
	return SUCCESS;
}

//----------------------------------------------------------------------
//	board_disable_intrs
//----------------------------------------------------------------------
void board_disable_intrs(void)
{
	unsigned long flags;

	spin_lock_irqsave(&tosca_lock, flags);

	tosca_intr_disabled = TRUE;

	spin_unlock_irqrestore(&tosca_lock, flags);
}

//----------------------------------------------------------------------
//	board_enable_intrs
//----------------------------------------------------------------------
void board_enable_intrs(void)
{
	unsigned long flags;

	spin_lock_irqsave(&tosca_lock, flags);

	if (tosca_intr_disabled) {
		tosca_intr_disabled = FALSE;
		wake_up(&tosca_intr_wait);
	}

	spin_unlock_irqrestore(&tosca_lock, flags);
}


//======================================================================
// rAPI support
// The reduced API used by the MSW
//=====================================================================

void do_rapi_lock(const char *func)
{
	INFO("Entering do rapi lock\n");
	if (down_trylock(&rapi_thread_lock)) {
		DBG(1,"%s: down_trylock lock failed,task=%s\n",func,current->comm);
		down(&rapi_thread_lock);
	}
	INFO("Exiting do rapi lock\n");
}

void do_rapi_unlock(const char *func)
{
	up(&rapi_thread_lock);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
	if (rapi_thread_lock.count > 1) {
		DBG(RAPI_D,"%s: counter > 1 (%d),task=%s\n",func,rapi_thread_lock.count,current->comm);
	}
#else
	if (rapi_thread_lock.count.counter > 1) {
		DBG(RAPI_D,"%s: counter > 1 (%d),task=%s\n",func,rapi_thread_lock.count.counter,current->comm);
	}
#endif
}

// Check if the address is a valid RAPI object
static int inline is_valid(void *addr,DWORD type)
{
	struct rapi_heap *obj=addr;

	if (!obj) {
		DBG(RAPI_D,"freeing NULL\n");
		return 0;
	}
	if ( (obj < min_addr) || (obj > max_addr)) {
		DBG(RAPI_D,"freeing invalid address %p,min_addr=%p,max_addr=%p\n",
				obj,min_addr,max_addr);
		return 0;
	}

	if (obj->type != type) {
		DBG(RAPI_D,"invalid object %p, type %lx,exp %lx\n",obj,obj->type,type);
		return 0;
	}
	// ok
	return 1;
}



// Allocate a new memory object
static void *alloc_obj(DWORD size,DWORD type)
{	
	struct rapi_heap *obj;
	if ((type > TIMER_TYPE) || (type < MEM_TYPE)) {
		DBG(RAPI_D,"illegal object type %lx\n",type);
		return NULL;
	}
	obj = kmalloc(size, type==MSG_TYPE ? GFP_ATOMIC : GFP_KERNEL);
	if (!obj) {
		DBG(RAPI_D,"kmalloc failed,size=%ld,type=%lx\n",size,type);
		return NULL;
	}
	if (obj > max_addr) max_addr = obj;
	if (obj < min_addr) min_addr = obj;
	++obj_counters[type-RAPI_SIGNATURE];
	tot_mem += size;
	if (tot_mem > max_mem) {
		if ((max_mem < MAX_MEM) && (tot_mem > MAX_MEM)) {
			DBG(RAPI_D,"total memory (%ld) exceeds max (%ld)\n",tot_mem,MAX_MEM);
		}
		max_mem = tot_mem;
	}

	obj->type = type;
	obj->size = size;

	return obj;
}

// Free the memory object
static void free_obj(void *addr)
{
	struct rapi_heap *obj = (struct rapi_heap *)addr;

	if (!obj) {
		return;
	}

	if ((obj->type > TIMER_TYPE) || (obj->type < MEM_TYPE)) {
		DBG(RAPI_D,"illegal object type %lx\n",obj->type);
		return;
	}
	--obj_counters[obj->type-RAPI_SIGNATURE];
	tot_mem -= obj->size;
	obj->type = 0;
	obj->size = 0;
	kfree(obj);		
}

// Allocate a new RAPI object
static void *new_object(DWORD size,DWORD type)
{
	unsigned long flags;	
	struct rapi_heap *obj;

	obj = alloc_obj(size,type);
	if (obj) {
		// add to list
		spin_lock_irqsave(&rapi_heap_lock, flags);
		list_add(&obj->list,&rapi_heap_list);
		spin_unlock_irqrestore(&rapi_heap_lock, flags);		
	}
	return obj;
}

// Delete the allocated RAPI object
static int del_obj(void *addr)
{
	unsigned long flags;	
	struct rapi_heap *obj = (struct rapi_heap *)addr;

	if (!obj) {
		return FAILURE;			
	}

	DBG(RAPI_D,"freeing object,addr=%lx,type=%lx\n",(long)obj,obj->type);

	switch(obj->type) {

		case MEM_TYPE:
			break;
		case TASK_TYPE:
			break;
		case SEM_TYPE:		
			// Stop semaphore timer
			{
				struct rapi_sem *s = (struct rapi_sem *)obj;

				if (s->timer.data) {
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0))
	                del_timer(&s->timer.t);
#else
	                del_timer(&s->timer);
#endif
					s->timer.data = 0L;
				}
			}
			break;
		case Q_TYPE:
			// Delete all messages on this q
			{
				DWORD tmp[4];

				while (xq_receive((DWORD)obj,1,0,tmp) == SUCCESS) {
				}		
			}		
			break;
		case TIMER_TYPE:
			{
			        struct rapi_timer *t = (struct rapi_timer *)obj;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0))
	                del_timer(&t->timer.t);
#else
	                del_timer(&t->timer);
#endif

			}
			break;
		case MSG_TYPE:	
		default:
			DBG(RAPI_D,"unknown object type %lx\n",obj->type);
			return FAILURE;			
			break;
	}
	spin_lock_irqsave(&rapi_heap_lock, flags);
	list_del(&obj->list);
	spin_unlock_irqrestore(&rapi_heap_lock, flags);

	free_obj(obj);

	return SUCCESS;
}

static struct rapi_task *find_task(struct task_struct *thread)
{	
	struct rapi_task *k,*ret;

	ret = NULL;
	for (k = (struct rapi_task *)rapi_heap_list.next;
			k != (struct rapi_task *)&rapi_heap_list;
			k = (struct rapi_task *)k->list.next
	    ) {
		if ((k->type==TASK_TYPE) && (k->thread == thread)) {
			ret = k;
			break;
		}
	}
	return ret;
}

static void rapi_exit_handler(void)
{
	struct rapi_task *k;


	if ((k = find_task(current))) {
		DBG(RAPI_D,"task %s killed\n",current->comm);
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,4,8))
		atomic_dec(&running_tasks);
		rapi_unlock();
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5,17,0))
        kthread_complete_and_exit(NULL,0);
#else
		complete_and_exit(NULL,0);
#endif
		ERR("\n");
		BUG();
#endif
	} else {
		DBG(RAPI_D,"thread %s not rAPI task\n",current->comm);
	}

}

//	Allocates a memory buffer
//	-------------------------
DWORD xm_getmem(
		DWORD	size,
		PVOID	*bufaddr
	       )
{
	struct rapi_mem *m;

	m = new_object(sizeof(struct rapi_heap) + size, MEM_TYPE);
	if (!m) {
		return FAILURE;
	}
	*bufaddr = &m->mem;
	DBG(RAPI_D,"size=%ld,bufaddr=%lx\n",size,(long)(*bufaddr));
	return SUCCESS;
}

//	Release a memory buffer
//	-----------------------
DWORD xm_retmem(
		PVOID bufaddr
	       )
{
	struct rapi_mem *m;

	DBG(RAPI_D,"bufaddr=%lx\n",(long)bufaddr);

	m = (struct rapi_mem *)((char *)bufaddr - sizeof(struct rapi_heap));	
	if (!is_valid(m,MEM_TYPE)) {
		return FAILURE;
	}

	return del_obj(m);
}

//	Creates a task
//	--------------
DWORD xt_create(
		char	name[4],
		DWORD	prio,
		DWORD	sstack,
		DWORD	ustack,
		DWORD	flags,
		DWORD	*tid
	       )
{
	struct rapi_task *k;

	DBG(RAPI_D,"\n");

	k = new_object(sizeof(struct rapi_task), TASK_TYPE);
	if (!k) {
		*tid = 0L;
		return FAILURE;
	}

	k->priority = prio;
	memcpy(k->name,name,4);
	k->start_addr = NULL;
	k->thread = NULL;
	*tid = (DWORD)k;
	return SUCCESS;
}

static int start_fn(void *arg)
{
	struct rapi_task *k = (struct rapi_task *)arg;

	if (!is_valid(k, TASK_TYPE)) {
		return FAILURE;
	}
	if (!k->start_addr) {
		return FAILURE;
	}

//      is this needed?
//	lock_kernel();
#if LINUX_VERSION_CODE >=  KERNEL_VERSION(2,6,0) && LINUX_VERSION_CODE < KERNEL_VERSION(3,8,0)
	daemonize("UNICORN");
#elseif LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
	exit_files(current); 
	daemonize();
#endif
	// Setup a nice name
	strcpy(current->comm, "UNICORN-");
	strncat(current->comm, k->name, 4);

	if (k->priority > XPRIO_BACKGRND_APPL) {
		int adj = (k->priority-XPRIO_BACKGRND_APPL) / 20;
#ifdef DEF_NICE
		current->nice -= adj;
#else
		// Preemptive kernel (2.6 for example)
		set_user_nice(k->thread,-adj);
#endif
	}
//      is this needed?
//	unlock_kernel();

	DBG(RAPI_D,"start %.4s\n",k->name);

	atomic_inc(&running_tasks);

	rapi_lock();
	k->start_addr(k->args[0], k->args[1], k->args[2], k->args[3]);
	rapi_unlock();
	DBG(RAPI_D,"exit %.4s\n",k->name);
	atomic_dec(&running_tasks);
	return SUCCESS;
}

//	Starts a task
//	-------------
DWORD xt_start(
		DWORD		tid,
		DWORD		mode,
		START_FUNC	start_addr,
		DWORD		args[4]
	      )
{
	struct rapi_task *k = (struct rapi_task *)tid;
//	int pid;

	if (!is_valid(k, TASK_TYPE)) {
		return FAILURE;			
	}

	k->start_addr = start_addr;
	memcpy(k->args,args,sizeof(k->args));

/*
	pid = kernel_thread(start_fn, k, CLONE_FS | CLONE_FILES | CLONE_SIGHAND);
	if (pid <= 0) {
		DBG(RAPI_D,"kernel_thread failed\n");
		del_obj(k);
		return FAILURE;
	}
	k->thread = find_task_by_pid(pid);
	if (k->thread == NULL) {
		DBG(RAPI_D,"kernel_thread failed\n");
		del_obj(k);
		return FAILURE;
	}
*/

        k->thread = kthread_run(start_fn, k, "rapi_task_thread_%d", CLONE_FS | CLONE_FILES | CLONE_SIGHAND);
        if (IS_ERR(k->thread)) {
                int rc = PTR_ERR(k->thread);
                DBG(RAPI_D, "rapi_task_thread failed: %d", rc);
        }
//#ifdef CONFIG_SMP
	// Hack to try to make it work on SMP !!!!
	// Lock the threads to CPU #0
	//set_cpus_allowed(k->thread,1UL << 0);
//#endif
	return SUCCESS;	
}

//	Disables task scheduling
//	------------------------
void xt_entercritical(void)
{
	DBG(RAPI_D,"\n");
}


//	Enables task scheduling
//	-----------------------
void xt_exitcritical(void)
{
	DBG(RAPI_D,"\n");
}

//	Waits until all tasks terminate
//	-------------------------------
void xt_waitexit(void)
{
	unsigned int count;

	DBG(1,"running_tasks=%d\n",running_tasks.counter);

	// Wait until all threads have terminated
	if(GlobalRemove == TRUE) {
		count = 2;
	} else {
		count = 20;
	}

	while (atomic_read(&running_tasks) && count--) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,14,0) 
	    current->__state = TASK_INTERRUPTIBLE;
#else
	    current->state = TASK_INTERRUPTIBLE;
#endif
		schedule_timeout(HZ);
	}

	DBG(1,"exit,running_tasks=%d\n",running_tasks.counter);
}

//	Creates a semaphore
//	-------------------
DWORD xsm_create(
		char	name[4],
		DWORD	count,
		DWORD	flags,
		DWORD	*smid
		)
{
	struct rapi_sem *s;

	s = new_object(sizeof(struct rapi_sem), SEM_TYPE);
	if (!s) {
		*smid = 0L;
		return FAILURE;
	}

	memcpy(s->name,name,4);
	sema_init(&s->sem,count);
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0))
	timer_setup(&s->timer.t, legacy_timer_emu_func, 0);
#else
	init_timer(&s->timer);
#endif
	s->timer.data = 0L;
	*smid = (DWORD)s;
	DBG(RAPI_D,"sem=%.4s,count=%ld\n",s->name,count);
	return SUCCESS;
}

//	Gets a semaphore ident
//	----------------------
DWORD xsm_ident(
		char	name[4],
		DWORD	node,
		DWORD	*smid
	       )
{
	unsigned long flags;
	struct list_head *rapi_obj;
	struct rapi_sem *s;
	DWORD status = FAILURE;

	*smid = 0L;
	spin_lock_irqsave(&rapi_heap_lock, flags);
	for (rapi_obj = rapi_heap_list.next;
			rapi_obj != &rapi_heap_list;
			rapi_obj = rapi_obj->next) {
		s = (struct rapi_sem *)rapi_obj;
		if ((s->type == SEM_TYPE) && (memcmp(s->name,name,4)==0)) {
			*smid = (DWORD)s;
			status = 0;
			break;
		}

	}
	spin_unlock_irqrestore(&rapi_heap_lock, flags);

	DBG(RAPI_D,"name=%.4s,smid=%lx\n",name,*smid);
	return status;	
}

static void process_sem_timeout(unsigned long __data)
{
	struct task_struct * task_struct = (struct task_struct *) __data;

	DBG(RAPI_D,"task_struct=%p\n",task_struct);
	if (task_struct) {
		send_sig(SIGALRM,task_struct,1);
	}
}

//	Allows a task to acquire a semaphore
//	------------------------------------
DWORD xsm_p(
		DWORD	smid,
		DWORD	no_wait,
		DWORD	timeout
	   )
{
	DWORD status;
	int failed;
	struct rapi_sem *s = (struct rapi_sem *)smid;

	if (GlobalRemove) {
		rapi_exit_handler();
		return FAILURE;			
	}

	if (!is_valid(s, SEM_TYPE)) {
		return FAILURE;			
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
	DBG(RAPI_D,"sem=%.4s,count=%d,timeout=%ldms\n",
			s->name,s->sem.count,no_wait ? 0 : timeout);
#else
	DBG(RAPI_D,"sem=%.4s,count=%d,timeout=%ldms\n",
			s->name,s->sem.count.counter,no_wait ? 0 : timeout);
#endif

	status = SUCCESS; 
	if (down_trylock(&s->sem)) {
		if (no_wait) {
			status = FAILURE;
		} else {
			if (timeout) {
				if (s->timer.data == 0) {
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0))
	                timer_setup(&s->timer.t, legacy_timer_emu_func, 0);
	                s->timer.t.expires = ((timeout*HZ)/1000) + jiffies;
#else
	                init_timer(&s->timer);
	                s->timer.expires = ((timeout*HZ)/1000) + jiffies;
#endif		
					s->timer.data = (unsigned long) current;
					s->timer.function = process_sem_timeout;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0))
	                add_timer(&s->timer.t);
#else
	                add_timer(&s->timer);
#endif
				} else {
					DBG(RAPI_D,"%.4s timer already running\n",s->name);
				}
			}
			rapi_unlock();
			failed = down_interruptible(&s->sem);
			rapi_lock();
			if (failed) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
				DBG(RAPI_D,"down_interruptible failed,sem=%.4s,count=%d\n",s->name,s->sem.count);
#else
				DBG(RAPI_D,"down_interruptible failed,sem=%.4s,count=%d\n",s->name,s->sem.count.counter);
#endif
				status = FAILURE;
				// has a timeout occured ?
				if (sigismember(&current->pending.signal,SIGALRM)) {
					// clear signals
					flush_signals(current);
				} else {
					// process killed
					if (timeout) {
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0))
	                    del_timer(&s->timer.t);
#else
	                    del_timer(&s->timer);
#endif
						s->timer.data = 0L;
					}
					rapi_exit_handler();
				}
			}
			if (timeout) {
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0))
	            del_timer(&s->timer.t);
#else
	            del_timer(&s->timer);
#endif
				s->timer.data = 0L;
			}

			if (GlobalRemove ) {
				rapi_exit_handler();
				return FAILURE;
			}

		}
	}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
	DBG(RAPI_D,"sem=%.4s,count=%d,status=%ld\n",s->name,s->sem.count,status);
#else
	DBG(RAPI_D,"sem=%.4s,count=%d,status=%ld\n",s->name,s->sem.count.counter,status);
#endif
	return status;
}

//	Releases a semaphore
//	--------------------
DWORD xsm_v(
		DWORD	smid
	   )
{
	struct rapi_sem *s = (struct rapi_sem *)smid;

	if (!is_valid(s, SEM_TYPE)) {
		return FAILURE;			
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
	DBG(RAPI_D,"sem=%.4s,count=%d\n", s->name,s->sem.count);
#else
	DBG(RAPI_D,"sem=%.4s,count=%d\n", s->name,s->sem.count.counter);
#endif

	up(&s->sem);

	return SUCCESS;
}

//	Creates a message queue
//	-----------------------
DWORD xq_create(
		char	name[4],
		DWORD	count,
		DWORD	flags,
		DWORD	*qid
	       )
{
	struct rapi_q *q;

	*qid = 0L;
	q = new_object(sizeof(struct rapi_q), Q_TYPE);
	if (!q) {
		*qid = 0L;
		return FAILURE;
	}
	if (xsm_create(name,0,0,&q->q_sem) != SUCCESS) {
		del_obj(q);
		return FAILURE;
	}

	memcpy(q->name,name,4);
//	q->msg_q_lock = SPIN_LOCK_UNLOCKED;
	spin_lock_init(&q->msg_q_lock);
//	q->msg_q_lock =  __SPIN_LOCK_UNLOCKED();
	INIT_LIST_HEAD(&q->msg_q);	
	*qid = (DWORD)q;
	DBG(RAPI_D,"q=%.4s\n",q->name);
	return SUCCESS;
}

static void handle_timer_msg(DWORD timer_id)
{
	unsigned long flags;
	struct rapi_timer *t = (struct rapi_timer *)timer_id;

	spin_lock_irqsave(&rapi_timer_lock, flags);

	if (!is_valid(t,TIMER_TYPE)) {
		goto exit;
	}

	DBG(RAPI_D,"tmid=%p,interval=%ldms,mode=%ld,userdata=%lx\n",t,t->interval,t->mode,t->userdata);

	if (t->mode == E_XTM_ONE_SHOT) {
		del_obj(t);
	} else {
		// restart
		
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0))
	    mod_timer(&t->timer.t,((t->interval*HZ)/1000)+jiffies);
#else
	    mod_timer(&t->timer,((t->interval*HZ)/1000)+jiffies);
#endif
	}
exit:
	spin_unlock_irqrestore(&rapi_timer_lock, flags);
}

//	Receives a message from a queue
//	-------------------------------
DWORD xq_receive(
		DWORD	qid,
		DWORD	no_wait,
		DWORD	timeout,
		DWORD	msg_buf[4]
		)
{
	unsigned long flags;
	struct rapi_q *q = (struct rapi_q *)qid;
	struct rapi_msg *msg;
	int i;
	DWORD status;

	if (GlobalRemove) rapi_exit_handler();

	if (!is_valid(q, Q_TYPE)) {
		return FAILURE;			
	}

	DBG(RAPI_D,"q=%.4s,timeout=%ldms\n",q->name,no_wait ? 0 : timeout);

	status = FAILURE;
	if (xsm_p(q->q_sem, no_wait, timeout) == SUCCESS) {	
		spin_lock_irqsave(&q->msg_q_lock, flags);		
		if (!list_empty(&q->msg_q)) {
			msg = (struct rapi_msg *)q->msg_q.next;
			list_del_init(&msg->list);

			for (i=0; i < 4; i++) 
				msg_buf[i] = msg->msg_buf[i];

			// delete or restart timer
			if (msg->timer_id) {
				handle_timer_msg(msg->timer_id);
			} 

			// Free message buffer
			free_obj(msg);
			DBG(RAPI_D,"msg_buf=%lx %lx %lx %lx\n",
					msg_buf[0],msg_buf[1],msg_buf[2],msg_buf[3]);
			status = SUCCESS;
		} else {
			spin_unlock_irqrestore(&q->msg_q_lock, flags);
			for (i=0; i < 4; i++) 
				msg_buf[i] = 0L;
			DBG(RAPI_D,"no msg\n");

		}	
		spin_unlock_irqrestore(&q->msg_q_lock, flags);		
	}
	return status;
}

static int msg_exists(struct rapi_q *q,DWORD timer_id)
{
	struct list_head *tmp;

	if (timer_id == 0) {
		return 0;
	}
	for (tmp = q->msg_q.next;
			tmp != &q->msg_q;
			tmp = tmp->next) {
		struct rapi_msg *msg = (struct rapi_msg *)tmp; 
		if (timer_id == msg->timer_id) {
			return 1;
		}
	}
	return 0;
}

static int put_msg(DWORD qid,DWORD msg_buf[4],DWORD timer_id)
{
	unsigned long flags;
	struct rapi_q *q = (struct rapi_q *)qid;
	struct rapi_msg *msg;
	int status;
	int i;

	if (!is_valid(q, Q_TYPE)) {
		return FAILURE;			
	}

	DBG(RAPI_D,"q=%.4s,msg_buf=%lx %lx %lx %lx\n",
			q->name,msg_buf[0],msg_buf[1],msg_buf[2],msg_buf[3]);

	spin_lock_irqsave(&q->msg_q_lock, flags);	

	if (msg_exists(q,timer_id)) {
		DBG(RAPI_D,"duplicate timer %lx",timer_id);
		status = FAILURE;
		goto exit;
	}

	// Allocate message buffer	
	msg = alloc_obj(sizeof(struct rapi_msg),MSG_TYPE);
	if (!msg) {
		status = FAILURE;
		goto exit;
	}

	for (i=0; i < 4; i++) 
		msg->msg_buf[i] = msg_buf[i];

	msg->timer_id = timer_id;

	list_add_tail(&msg->list,&q->msg_q);
	xsm_v(q->q_sem); // Wake up task waiting for message

	status = SUCCESS;
exit:
	spin_unlock_irqrestore(&q->msg_q_lock, flags);
	return status;
}

//	Sends a message to a queue
//	--------------------------
DWORD xq_send(
		DWORD	qid,
		DWORD	msg_buf[4]
	     )
{
	return put_msg(qid, msg_buf, 0);
}

//----------------------------------------------------------------------
//	TIMER MANAGEMENT
//----------------------------------------------------------------------
#define	XMID_TIMER_EX	1

#define GET_XMID_TIMER(msg_ptr) \
	(*(unsigned short *)((unsigned long)msg_ptr))

static void del_timer_msg(DWORD qid,DWORD timer_id)
{
	unsigned long flags;
	struct rapi_q *q = (struct rapi_q *)qid;
	struct list_head *tmp;

	spin_lock_irqsave(&q->msg_q_lock, flags);		
	for (tmp = q->msg_q.next;
			tmp != &q->msg_q;
			tmp = tmp->next) {
		struct rapi_msg *msg = (struct rapi_msg *)tmp; 
		if (timer_id == msg->timer_id) {
			xsm_p(q->q_sem,1,0);
			DBG(RAPI_D,"msg=%p,timer %lx,userdata=%lx\n",msg,timer_id,msg->msg_buf[3]);
			list_del(&msg->list);
			free_obj(msg);
			break;
		}
	}
	spin_unlock_irqrestore(&q->msg_q_lock, flags);		
}

static void timer_callback(unsigned long __data)
{
	unsigned long flags;
	struct rapi_timer *t = (struct rapi_timer *) __data;
	DWORD msg_buf[4];

	if (GlobalRemove) return;

	spin_lock_irqsave(&rapi_timer_lock, flags);

	if (!is_valid(t, TIMER_TYPE)) {
		DBG(RAPI_D,"timer %p not valid\n",t);
		goto exit;
	}

	DBG(RAPI_D,"tmid=%p,interval=%ldms,mode=%ld,userdata=%lx\n",
			t,t->interval,t->mode,t->userdata);	

	msg_buf[0] = 0;
	GET_XMID_TIMER(msg_buf) = XMID_TIMER_EX;
	msg_buf[1] = (DWORD)t;
	msg_buf[2] = 0;
	msg_buf[3] = t->userdata;
	put_msg(t->qid, msg_buf, (DWORD)t);
exit:	
	spin_unlock_irqrestore(&rapi_timer_lock, flags);
}



//	Starts a timer
//	--------------
DWORD xtm_startmsgtimer(
		DWORD	qid,
		T_timer_mode mode,
		DWORD	interval,
		DWORD	userdata,
		BYTE	owner_ctrl,
		DWORD	*tmid
		)
{
	struct rapi_timer *t;

	if (GlobalRemove) rapi_exit_handler();

	*tmid = 0L;

	mode &= 0xFFFF;
	switch (mode) {
		case E_XTM_ONE_SHOT:
		case E_XTM_PERIODIC:
			break;
		default:
			DBG(RAPI_D,"mode not valid\n");
			return FAILURE;
	}

	t = new_object(sizeof(struct rapi_timer), TIMER_TYPE);
	if (!t) {
		return FAILURE;
	}
	DBG(RAPI_D,"tmid=%lx,qid=%lx,interval=%ldms,mode=%d,userdata=%lx\n",(DWORD)t,qid,interval,mode,userdata);
	t->qid = qid;
	t->mode = mode;
	t->interval = interval;
	t->userdata = userdata;
	t->owner = owner_ctrl ? current : 0;
	*tmid = (DWORD)t; 

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0))
	timer_setup(&t->timer.t, legacy_timer_emu_func, 0);
	t->timer.t.expires = ((interval*HZ)/1000) + jiffies;
#else
	init_timer(&t->timer);
	t->timer.expires = ((interval*HZ)/1000) + jiffies;
#endif
	t->timer.data = (unsigned long)t;
	t->timer.function = timer_callback;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0))
	add_timer(&t->timer.t);
#else
	add_timer(&t->timer);
#endif

	return SUCCESS;
}

//	Stops a timer
//	-------------
DWORD xtm_stopmsgtimer(
		DWORD	tmid
		)
{
	unsigned long flags;
	struct rapi_timer *t = (struct rapi_timer *)tmid;
	int status;

	if (GlobalRemove) rapi_exit_handler();

	DBG(RAPI_D,"tmid=%lx\n",tmid);

	spin_lock_irqsave(&rapi_timer_lock, flags);

	if (!is_valid(t, TIMER_TYPE)) {
		DBG(RAPI_D,"timer %p not valid\n",t);
		status = FAILURE;
		goto exit;
	}

	if (t->owner) {
		if (t->owner != current) {
			DBG(RAPI_D,"not owner of timer\n");
			status = FAILURE;	
			goto exit;
		}
	}

	status = SUCCESS;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0))
	del_timer(&t->timer.t);
#else
	del_timer(&t->timer);
#endif
	del_timer_msg(t->qid,tmid);
	del_obj(t);
exit:	
	spin_unlock_irqrestore(&rapi_timer_lock, flags);	
	return status;
}

//	Wake up after n milliseconds
//	----------------------------
DWORD xtm_wkafter(DWORD ms)
{
	signed long ticks;

	DBG(RAPI_D,"timeout=%ldms\n",ms);

	if (GlobalRemove) rapi_exit_handler();

	ticks = (ms*HZ)/1000;

	rapi_unlock();
	if (ticks > 0) {
		// sleep
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,14,0) 
	    current->__state = TASK_INTERRUPTIBLE;
#else
	    current->state = TASK_INTERRUPTIBLE;
#endif
		schedule_timeout(ticks);
	} else {
		// busy loop
		mdelay(ms);
	}
	rapi_lock();

	if (GlobalRemove || signal_pending(current)) {
		rapi_exit_handler();
		return FAILURE;
	}

	return SUCCESS;
}

//	Returns system time in ms.
//	--------------------------
DWORD xtm_gettime(
		void
		)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0) 
    struct timespec64 ts;
    ktime_get_real_ts64(&ts);
    return ((ts.tv_sec*1000)+(ts.tv_nsec/1000000));
#else
    struct timeval tv;

	do_gettimeofday(&tv);
	return ((tv.tv_sec*1000)+(tv.tv_usec/1000));
#endif
}

//	Returns in ms the difference between two times
//	----------------------------------------------
DWORD xtm_timediff(
		DWORD time1,
		DWORD time2
		)
{
	return (time2 - time1);
}

//	Returns the elapsed time since the given time
//	---------------------------------------------
DWORD xtm_elapse(
		DWORD time
		)
{
	return (xtm_gettime() - time);
}

//	Gets a time stamp in ms and s.
//	-----------------------------
DWORD xtm_gettimestamp(
		DWORD *microseconds,
		DWORD *seconds
		)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0) 
    struct timespec64 ts;
    ktime_get_real_ts64(&ts);
    *seconds = ts.tv_sec;
    *microseconds = ts.tv_nsec*1000;
    return SUCCESS;
#else 
    struct timeval tv;

	do_gettimeofday(&tv);
	*microseconds = tv.tv_usec;
	*seconds = tv.tv_sec;
	return SUCCESS;
#endif
	
}

extern int rapi_init(void)
{
	DBG(1,"HZ=%d\n",HZ);
	INFO("rapi init called\n");

	// create rAPI thread lock
	#if ( LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,37) )
		sema_init(&rapi_thread_lock,1);
	#else
		init_MUTEX(&rapi_thread_lock);
	#endif
	
	// Create C++ static classes 
	__do_global_ctors();

	start_time = xtm_gettime();
	DBG(1,"start_time=%ld\n",start_time);
	return SUCCESS;
}

static void free_all(DWORD type)
{
	struct list_head *rapi_obj;
	struct rapi_heap *tmp;

	DBG(RAPI_D,"type=%lx\n",type);

	for (rapi_obj = rapi_heap_list.next;
			rapi_obj != &rapi_heap_list;
	    ) {
		tmp = (struct rapi_heap *)rapi_obj;
		rapi_obj = rapi_obj->next;
		if ((type==0L) || (tmp->type == type)) {
			del_obj(tmp);
		}
	}
}

extern void rapi_exit(void)
{
	DBG(1,"tot_mem=%ld,max_mem=%ld bytes\n",tot_mem,max_mem);
	DBG(1,"obj_counters,MEM=%ld,TASK=%ld,SEM=%ld,Q=%ld,MSG=%ld,TIMER=%ld\n",
			obj_counters[0],obj_counters[1],obj_counters[2],
			obj_counters[3],obj_counters[4],obj_counters[5]);

	__do_global_dtors();

	board_disable_intrs();
	board_reset_intr_handler(tosca_intr_set);

	//GlobalRemove = TRUE;

	xt_waitexit();

	// free the objects in a certain order


	free_all(TASK_TYPE);
	free_all(TIMER_TYPE);
	free_all(Q_TYPE);
	free_all(SEM_TYPE);
	free_all(MSG_TYPE);
	free_all(MEM_TYPE);
	free_all(0L);

	INIT_LIST_HEAD(&rapi_heap_list);
}
