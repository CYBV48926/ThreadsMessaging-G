/* ------------------------------------------------------------------------
   Messaging.c
   College of Applied Science and Technology
   The University of Arizona
   CYBV 489

   Student Names:  <add your group members here>

   ------------------------------------------------------------------------ */
#include <Windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <THREADSLib.h>
#include <Scheduler.h>
#include <Messaging.h>
#include <stdint.h>
#include "message.h"

/* ------------------------- Prototypes ----------------------------------- */
static void nullsys(system_call_arguments_t* args);

/* Note: interrupt_handler_t is already defined in THREADSLib.h with the signature:
 *   void (*)(char deviceId[32], uint8_t command, uint32_t status, void *pArgs)
 */

static void InitializeHandlers();
static int check_io_messaging(void);
extern int MessagingEntryPoint(void*);
static void checkKernelMode(const char* functionName);

struct psr_bits {
    unsigned int cur_int_enable : 1;
    unsigned int cur_mode : 1;
    unsigned int prev_int_enable : 1;
    unsigned int prev_mode : 1;
    unsigned int unused : 28;
};

union psr_values {
    struct psr_bits bits;
    unsigned int integer_part;
};


/* -------------------------- Globals ------------------------------------- */

/* Obtained from THREADS*/
interrupt_handler_t* handlers;

/* system call array of function pointers */
void (*systemCallVector[THREADS_MAX_SYSCALLS])(system_call_arguments_t* args);

/* the mail boxes */
MailBox mailboxes[MAXMBOX];
MailSlot mailSlots[MAXSLOTS];

typedef struct
{
    void* deviceHandle;
    int deviceMbox;
    int deviceType;
    char deviceName[16];
} DeviceManagementData;

static DeviceManagementData devices[THREADS_MAX_DEVICES];
static int nextMailboxId = 0;
static int waitingOnDevice = 0;


/* ------------------------------------------------------------------------
     Name - SchedulerEntryPoint
     Purpose - Initializes mailboxes and interrupt vector.
               Start the Messaging test process.
     Parameters - one, default arg passed by k_spawn that is not used here.
----------------------------------------------------------------------- */
int SchedulerEntryPoint(void* arg)
{
    // TODO: check for kernel mode
	uint32_t psr = get_psr(); //checking the PSR for kernel mode
    int kernelMode = psr & PSR_KERNEL_MODE != 0;
    if (!kernelMode)
    {
        console_output(FALSE, "SchedulerEntryPoint called in kernel mode. Halting...\n");
        stop(1);
    }

    /* Disable interrupts */
    disableInterrupts();

    /* set this to the real check_io function. */
    check_io = check_io_messaging;

    /* Initialize the mail box table, slots, & other data structures.
     * Initialize int_vec and sys_vec, allocate mailboxes for interrupt
     * handlers.  Etc... */
	for (int i = 0; i < MAXMBOX; ++i) //initializing the mailboxes in the mailbox table
    {
		mailboxes[i].mbox_id = -1; //indicates that the mailbox is not in use
		mailboxes[i].slotCount = 0; //indicates the number of slots in the mailbox, 0 for zero-slot mailbox
		mailboxes[i].slotSize = 0; //indicates the size of each slot in the mailbox, 0 for zero-slot mailbox
		mailboxes[i].status = MBSTATUS_EMPTY; //indicates that the mailbox is empty and not in use
		mailboxes[i].type = MB_ZEROSLOT; //indicates the type of mailbox, defaulting to zero-slot until created with mailbox_create
        mailboxes[i].pSlotListHead = NULL;
        
    }

     /* Initialize the devices and their mailboxes. */

     /* Allocate mailboxes for use by the interrupt handlers.
      * Note: The clock device uses a zero-slot mailbox, while I/O devices
      * (disks, terminals) need slotted mailboxes since their interrupt
      * handlers use non-blocking sends.
      */
      // TODO: Create mailboxes for each device.
	devices[THREADS_CLOCK_DEVICE_ID].deviceMbox = mailbox_create(0, sizeof(int)); // clock device uses a zero-slot mailbox
	for (int i = 0; i < THREADS_MAX_DEVICES; i++) // create mailboxes for I/O devices, which use slotted mailboxes
    {
		if (i == THREADS_CLOCK_DEVICE_ID) continue; // already created mailbox for clock device
        devices[i].deviceMbox = mailbox_create(1, sizeof(int)); 
    }
    /* TODO: Initialize the devices using device_initialize().
     * The devices are: disk0, disk1, term0, term1, term2, term3.
     * Store the device handle and name in the devices array.
     */
    
    InitializeHandlers();

    enableInterrupts();
    
    /* TODO: Create a process for Messaging, then block on a wait until messaging exits.*/
    int result = k_spawn("Messaging", MessagingEntryPoint, NULL, 4 * THREADS_MIN_STACK_SIZE , 5);
	if (result < 0)//checking if the process was created successfully
    {
        console_output(FALSE, "Failed to spawn Messaging process. Halting...\n");
        stop(1);
    }
    
	int exitcode = 0; // variable to store the exit code of the Messaging process
	k_wait(&exitcode); // wait for the Messaging process to finish and store its exit code in exitcode
    
	k_exit(0); // exit the SchedulerEntryPoint process with a success code

    return 0;
} /* SchedulerEntryPoint */


/* ------------------------------------------------------------------------
   Name - mailbox_create
   Purpose - gets a free mailbox from the table of mailboxes and initializes it
   Parameters - maximum number of slots in the mailbox and the max size of a msg
                sent to the mailbox.
   Returns - -1 to indicate that no mailbox was created, or a value >= 0 as the
             mailbox id.
   ----------------------------------------------------------------------- */
int mailbox_create(int slots, int slot_size)
{
	for (int i = 0; i < MAXMBOX; ++i) //searching for a free mailbox in the mailbox table
    {
		if (mailboxes[i].status != MBSTATUS_INUSE) // found a free mailbox not in use
        {
			mailboxes[i].mbox_id = i; // assign the mailbox id to the index in the mailbox table
			mailboxes[i].slotCount = slots; // set the number of slots in the mailbox, 0 for zero-slot mailbox
            mailboxes[i].slotSize = slot_size;
			mailboxes[i].status = MBSTATUS_INUSE; // set the status of the mailbox to in use
			mailboxes[i].type = (slots == 0) ? MB_ZEROSLOT : (slots == 1 ? MB_SINGLESLOT : MB_MULTISLOT); // set the type of mailbox based on the number of slots
			mailboxes[i].pSlotListHead = NULL; // initialize the head of the slot list to NULL
			return i; // return the mailbox id
        }
       
    }
    return -1;
} /* mailbox_create */


/* ------------------------------------------------------------------------
   Name - mailbox_send
   Purpose - Put a message into a slot for the indicated mailbox.
             Block the sending process if no slot available.
   Parameters - mailbox id, pointer to data of msg, # of bytes in msg,
                block flag.
   Returns - zero if successful, -1 if invalid args, -2 if would block
             (non-blocking mode), -5 if signaled while waiting.
   Side Effects - none.
   ----------------------------------------------------------------------- */
 int mailbox_send(int mboxId, void* pMsg, int msg_size, int wait)
{
    // Validate mailbox id
     if (mboxId < 0 || mboxId >= MAXMBOX || mailboxes[mboxId].status != MBSTATUS_INUSE) //checking if the mailbox id is valid and the mailbox is in use
     {
         return -1;
     }
    
	 if (msg_size > mailboxes[mboxId].slotSize)//checking if the message size is greater than the slot size of the mailbox
     {
         return -1;
     }
    
	 int slotCount = 0; //count the number of slots currently in use for the mailbox
	 SlotPtr slot = mailboxes[mboxId].pSlotListHead; //pointer to traverse the slot list of the mailbox
	 while (slot) // counting the number of slots currently in use for the mailbox
    {
        slotCount++;
        slot = slot->pNextSlot;
    }
	 while (slotCount >= mailboxes[mboxId].slotCount) // checking if there are no available slots in the mailbox
    {
        
		 if (!wait) // if the block flag is not set, return -2 to indicate that the send would block
         {
             return -2;
         }
        
		 int pid = k_getpid(); // get the process id of the sending process to block it until a slot is available
		 WaitingProcessPtr wp = (WaitingProcessPtr)malloc(sizeof(struct waiting_process)); // create a waiting process structure to add the sending process to the waiting senders list of the mailbox 
        wp->pid = pid;
        wp->pNextProcess = NULL;
        wp->pPrevProcess = NULL; 
		if (!mailboxes[mboxId].waitingSendersTail) // if the waiting senders list is empty, set the head and tail to the new waiting process
        {
            mailboxes[mboxId].waitingSendersHead = wp;
            mailboxes[mboxId].waitingSendersTail = wp;
        }
		else // if the waiting senders list is not empty, add the new waiting process to the end of the list and update the tail pointer
        {
            mailboxes[mboxId].waitingSendersTail->pNextProcess = wp;
            wp->pPrevProcess = mailboxes[mboxId].waitingSendersTail;
            mailboxes[mboxId].waitingSendersTail = wp;

        }
        block(BLOCKED_SEND); // block the processs untill a slot is availble
        slotCount = 0;
		slot = mailboxes[mboxId].pSlotListHead; // after being unblocked, count the number of slots currently in use for the mailbox again to check if a slot is now available
        while (slot)
        {
            slotCount++;
            slot = slot->pNextSlot;
        }
    }
    
	 SlotPtr newSlot = (SlotPtr)malloc(sizeof(MailSlot)); //allocate a new slot for the message to be sent
     if (!newSlot)
     {
		 return -1; // if memory allocation for the new slot fails, return -1 to indicate an error
     }
	 newSlot->mbox_id = mboxId; // set the mailbox id of the new slot to the id of the mailbox it is being sent to
	 memcpy(newSlot->message, pMsg, msg_size); // copy the message data from the provided pointer to the message field of the new slot
	 newSlot->messageSize = msg_size; // set the message size of the new slot to the size of the message being sent
	 newSlot->pNextSlot = NULL; // initialize the next slot pointer of the new slot to NULL since it will be added to the end of the slot list of the mailbox
     newSlot->pPrevSlot = NULL;
    
	 if (!mailboxes[mboxId].pSlotListHead) // if the slot list of the mailbox is empty, set the head to the new slot
    {
        mailboxes[mboxId].pSlotListHead = newSlot;
    }
	 else // if the slot list of the mailbox is not empty, add the new slot to the end of the list and update the next and previous pointers accordingly
     {
		 SlotPtr last = mailboxes[mboxId].pSlotListHead; // pointer to traverse the slot list of the mailbox to find the last slot
        while (last->pNextSlot)
			last = last->pNextSlot; // traverse the slot list to find the last slot
		last->pNextSlot = newSlot; // set the next slot pointer of the last slot to the new slot
		newSlot->pPrevSlot = last; // set the previous slot pointer of the new slot to the last slot
     }
   
    return 0;
 }

/* ------------------------------------------------------------------------
   Name - mailbox_receive
   Purpose - Receive a message from the indicated mailbox.
             Block the receiving process if no message available.
   Parameters - mailbox id, pointer to buffer for msg, max size of buffer,
                block flag.
   Returns - size of received msg (>=0) if successful, -1 if invalid args,
             -2 if would block (non-blocking mode), -5 if signaled.
   Side Effects - none.
   ----------------------------------------------------------------------- */
 int mailbox_receive(int mboxId, void* pMsg, int msg_size, int wait)
{
	 // Validate mailbox id // checking if the mailbox id is valid and the mailbox is in use
     if (mboxId < 0 || mboxId >= MAXMBOX || mailboxes[mboxId].status != MBSTATUS_INUSE)
     {
         return -1;
	 }
    // Check for available slot
    SlotPtr slot = mailboxes[mboxId].pSlotListHead;
    while (!slot) {
        // No message available
        if (!wait)
        {
            return -2;
        }
		int pid = k_getpid(); // get the pid of process to block it until a message is available in the mailbox
		WaitingProcessPtr wp = (WaitingProcessPtr)malloc(sizeof(struct waiting_process)); // create a waiting process structure to add the receiving process to the waiting receivers list of the mailbox
		wp->pid = pid; // set the pid of the waiting process to the pid of the receiving process
        wp->pNextProcess = NULL;
        wp->pPrevProcess = NULL;
		if (!mailboxes[mboxId].waitingReceiversTail) // if the waiting receivers list is empty, set the head and tail to the new waiting process
        {
            mailboxes[mboxId].waitingReceiversHead = wp;
            mailboxes[mboxId].waitingReceiversTail = wp;
        }
		else // if the waiting receivers list is not empty, add the new waiting process to the end of the list and update the tail pointer
        {
            mailboxes[mboxId].waitingReceiversTail->pNextProcess = wp;
            wp->pPrevProcess = mailboxes[mboxId].waitingReceiversTail;
            mailboxes[mboxId].waitingReceiversTail = wp;

        }
        
		block(BLOCKED_RECEIVE); // block the receiving process until a message is available in the mailbox

		slot = mailboxes[mboxId].pSlotListHead; // after being unblocked, check the head of the slot list of the mailbox again to see if a message is now available
    }
   
	int copySize = (slot->messageSize < msg_size) ? slot->messageSize : msg_size; //copy message data from the slot to the provided buffer
    memcpy(pMsg, slot->message, copySize);
    // Remove slot from list
	mailboxes[mboxId].pSlotListHead = slot->pNextSlot; // update the head of the slot list to the next slot in the list
    if (mailboxes[mboxId].pSlotListHead) // if there is a next slot in the list, update its previous pointer to NULL since it will now be the head of the list
    {
        mailboxes[mboxId].pSlotListHead->pPrevSlot = NULL;
    }
    free(slot);
	WaitingProcessPtr sender = mailboxes[mboxId].waitingSendersHead; // check if there are any waiting senders for the mailbox and unblock the first sender in the waiting senders list if there is one
	if (sender) // if there is a waiting sender, unblock it and remove it from the waiting senders list
    {
		unblock(sender->pid); // unblock the sender process
		mailboxes[mboxId].waitingSendersHead = sender->pNextProcess; // update the head of the waiting senders list to the next sender in the list
		if (mailboxes[mboxId].waitingSendersHead) // if there is a next sender in the list, update its previous pointer to NULL since it will now be the head of the list
        {
            mailboxes[mboxId].waitingSendersHead->pPrevProcess = NULL;
        }
		else // if there are no more waiting senders in the list, set the tail pointer to NULL as well
        {
            mailboxes[mboxId].waitingSendersTail = NULL;
        }
		free(sender); // free the memory allocated for the sender that was unblocked
    }
    return copySize;
}


/* ------------------------------------------------------------------------
   Name - mailbox_free
   Purpose - Frees a previously created mailbox. Any process waiting on
             the mailbox should be signaled and unblocked.
   Parameters - mailbox id.
   Returns - zero if successful, -1 if invalid args, -5 if signaled
             while closing the mailbox.
   ----------------------------------------------------------------------- */
int mailbox_free(int mboxId)
{
    int result = -1;
    return result;
}

/* ------------------------------------------------------------------------
   Name - wait_device
   Purpose - Waits for a device interrupt by blocking on the device's
             mailbox. Returns the device status via the status pointer.
   Parameters - device name string, pointer to status output.
   Returns - 0 if successful, -1 if invalid parameter, -5 if signaled.
   ----------------------------------------------------------------------- */
int wait_device(char* deviceName, int* status)
{
    int result = 0;
    uint32_t deviceHandle = -1;
    checkKernelMode("waitdevice");

    enableInterrupts();

    if (strcmp(deviceName, "clock") == 0)
    {
        deviceHandle = THREADS_CLOCK_DEVICE_ID;
    }
    else
    {
        deviceHandle = device_handle(deviceName);

    }

    if (deviceHandle >= 0 && deviceHandle < THREADS_MAX_DEVICES)
    {
        /* set a flag that there is a process waiting on a device. */
        waitingOnDevice++;
        mailbox_receive(devices[deviceHandle].deviceMbox, status, sizeof(int), TRUE);

        disableInterrupts();

        waitingOnDevice--;
    }
    else
    {
        console_output(FALSE, "Unknown device type.");
        stop(-1);
    }

    /* spec says return -5 if signaled. */
    if (signaled())
    {
        result = -5;
    }

    return result;
}


int check_io_messaging(void)
{
    if (waitingOnDevice)
    {
        return 1;
    }
    return 0;
}

static void InitializeHandlers()
{
    handlers = get_interrupt_handlers();

    /* TODO: Register interrupt handlers in the handlers array.
     * Use the interrupt indices defined in THREADSLib.h:
     *   handlers[THREADS_TIMER_INTERRUPT]   = your_clock_handler;
     *   handlers[THREADS_IO_INTERRUPT]      = your_io_handler;
     *   handlers[THREADS_SYS_CALL_INTERRUPT] = your_syscall_handler;
     *
     * Also initialize the system call vector (systemCallVector).
     */

}

/* an error method to handle invalid syscalls */
static void nullsys(system_call_arguments_t* args)
{
    console_output(FALSE,"nullsys(): Invalid syscall %d. Halting...\n", args->call_id);
    stop(1);
} /* nullsys */


/*****************************************************************************
   Name - checkKernelMode
   Purpose - Checks the PSR for kernel mode and halts if in user mode
   Parameters -
   Returns -
****************************************************************************/
static inline void checkKernelMode(const char* functionName)
{
    union psr_values psrValue;

    psrValue.integer_part = get_psr();
    if (psrValue.bits.cur_mode == 0)
    {
        console_output(FALSE, "Kernel mode expected, but function called in user mode.\n");
        stop(1);
    }
}
