/* ------------------------------------------------------------------------
Messaging.c
College of Applied Science and Technology
The University of Arizona
CYBV 489

Student Names : <add your group members here>

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
static void terminal_interrupt_handler(void* device, uint8_t command, uint32_t status, void* pArgs);
static void nullsys(system_call_arguments_t* args);

/* Note: interrupt_handler_t is already defined in THREADSLib.h with the signature:
 *   void (*)(char deviceId[32], uint8_t command, uint32_t status, void *pArgs)
 */

static void clock_interupt_handler(void* device, uint8_t command, uint32_t status, void* pArgs);
static void InitializeHandlers();
static int check_io_messaging(void);
extern int MessagingEntryPoint(void*);
static void checkKernelMode(const char* functionName);
static void syscall_handler(void* device, uint8_t command, uint32_t status, void* pArgs);
static void io_handler(void* device, uint8_t command, uint32_t status, void* pArgs); 

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

/* Global slot pool counter - tracks total slots in use across ALL mailboxes */
static int globalSlotsInUse = 0;

/* ------------------------------------------------------------------------
     Name - SchedulerEntryPoint
     Purpose - Initializes mailboxes and interrupt vector.
               Start the Messaging test process.
     Parameters - one, default arg passed by k_spawn that is not used here.
----------------------------------------------------------------------- */
int SchedulerEntryPoint(void* arg)
{
    uint32_t psr = get_psr(); // get the psr
	int kernelMode = (psr & PSR_KERNEL_MODE) != 0;// check if kernel mode bit is set
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
	for (int i = 0; i < THREADS_MAX_SYSCALLS; i++) // Initialize all system call vector entries to nullsys
    {
        systemCallVector[i] = nullsys;
    }

    // Initialize all mailbox table entries to empty
	for (int i = 0; i < MAXMBOX; ++i) // Initialize all mailbox entries to empty
    {
		mailboxes[i].mbox_id = -1; // -1 indicates invalid/uninitialized mailbox ID
		mailboxes[i].slotCount = 0; // zero-slot by default until created with mailbox_create
		mailboxes[i].slotSize = 0; // zero slot size by default until created with mailbox_create
		mailboxes[i].status = MBSTATUS_EMPTY; // mark all mailboxes as empty/available
		mailboxes[i].type = MB_ZEROSLOT; // default to zero-slot type until created with mailbox_create (type is determined by slotCount in mailbox_create)
        mailboxes[i].pSlotListHead = NULL; // head of the slot list for this mailbox
        mailboxes[i].waitingSendersHead = NULL; // head of the waiting senders list
        mailboxes[i].waitingSendersTail = NULL; // tail of the waiting senders list
        mailboxes[i].waitingReceiversHead = NULL; // head of the waiting receivers list
        mailboxes[i].waitingReceiversTail = NULL; // tail of the waiting receivers list
    }

    // Initialize all device entries
    for (int i = 0; i < THREADS_MAX_DEVICES; ++i)
    {
		devices[i].deviceHandle = NULL; // will be set to the device handle returned by device_initialize
		devices[i].deviceMbox = -1; // will be set to the mailbox ID created for the device
		devices[i].deviceType = 0; // 0=disk, 1=terminal - will be set based on the device type during initialization
		devices[i].deviceName[0] = '\0';  // will be set to the device name during initialization
    }

    // Clock device: zero-slot mailbox (synchronization rendezvous)
	devices[THREADS_CLOCK_DEVICE_ID].deviceMbox = mailbox_create(0, sizeof(int)); // zero-slot mailbox for clock device
	strcpy_s(devices[THREADS_CLOCK_DEVICE_ID].deviceName, // set clock device name
		sizeof(devices[THREADS_CLOCK_DEVICE_ID].deviceName), "clock"); // clock device is always at index 0

    // I/O devices (indices 1 through THREADS_MAX_DEVICES-1):
    // Create one slotted mailbox per slot regardless of physical device count.
    // This ensures the test's first mailbox_create always gets ID = THREADS_MAX_DEVICES (8).
    for (int i = 1; i < THREADS_MAX_DEVICES; ++i)
    {
        devices[i].deviceMbox = mailbox_create(5, sizeof(uint32_t));
    }

    // Now initialize the physical devices and map their handles to the correct slots
    const char* deviceNames[] = { "disk0", "disk1", "term0", "term1", "term2", "term3" };
    for (int i = 0; i < 6; ++i)
    {
        uint32_t handle = device_initialize((char*)deviceNames[i]);
        // handle returned IS the index into devices[] used by io_handler
		devices[handle].deviceHandle = (void*)(intptr_t)handle;// store the device handle (cast to void*) in the devices array at the index corresponding to the handle
        devices[handle].deviceType   = (i < 2) ? 0 : 1;  // 0=disk, 1=terminal
		strcpy_s(devices[handle].deviceName, // set device name in the devices array at the index corresponding to the handle
			sizeof(devices[handle].deviceName), deviceNames[i]); // copy the device name from the deviceNames array to the deviceName field
        
    }

    InitializeHandlers();

    enableInterrupts();

    /* TODO: Create a process for Messaging, then block on a wait until messaging exits.*/
    int result = k_spawn("Messaging", MessagingEntryPoint, NULL, 4 * THREADS_MIN_STACK_SIZE, 5);
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
    // Validate parameters
    // slots must be >= 0 (0 is valid for zero-slot rendezvous mailboxes)
    if (slots < 0) {
        return -1;
    }
    // slot_size must be >= 0 and must not exceed MAX_MESSAGE
    if (slot_size < 0 || slot_size > MAX_MESSAGE) {
        return -1;
    }

    for (int i = 0; i < MAXMBOX; ++i)
    {
        if (mailboxes[i].status != MBSTATUS_INUSE)
        {
			mailboxes[i].mbox_id = i; // assign mailbox ID based on index in the mailboxes array
			mailboxes[i].slotCount = slots; // set the slot count for this mailbox based on the slots parameter
			mailboxes[i].slotSize = slot_size; // set the slot size for this mailbox based on the slot_size parameter
			mailboxes[i].status = MBSTATUS_INUSE; // mark the mailbox as in use
			mailboxes[i].type = (slots == 0) ? MB_ZEROSLOT : (slots == 1 ? MB_SINGLESLOT : MB_MULTISLOT); // determine mailbox type based on slot count
            mailboxes[i].pSlotListHead = NULL; // initialize slot list head to NULL
            mailboxes[i].waitingSendersHead = NULL; // initialize waiting senders head to NULL
            mailboxes[i].waitingSendersTail = NULL; // initialize waiting senders tail to NULL
            mailboxes[i].waitingReceiversHead = NULL; // initialize waiting receivers head to NULL
            mailboxes[i].waitingReceiversTail = NULL; // initialize waiting receivers tail to NULL
            
            return i;
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
    disableInterrupts();
    
    // Validate mailbox id
    if (mboxId < 0 || mboxId >= MAXMBOX || mailboxes[mboxId].status != MBSTATUS_INUSE) 
    {
        enableInterrupts();
        return -1;
    }
	if (msg_size > mailboxes[mboxId].slotSize) // Validate message size against mailbox's slot size
    {
        enableInterrupts();
        return -1;
    }
    
    // Zero-slot mailbox
    if (mailboxes[mboxId].slotCount == 0) 
    {
        // If a receiver is waiting, unblock and deliver
		WaitingProcessPtr receiver = mailboxes[mboxId].waitingReceiversHead; // check if there is a waiting receiver for this mailbox
        if (receiver) {
            // Deliver directly to waiting receiver
			if (receiver->msgBuffer && pMsg) // check if the receiver's message buffer and the sender's message pointer are valid before copying
            {
                int copySize = (msg_size < receiver->msgSize) ? msg_size : receiver->msgSize;
                memcpy(receiver->msgBuffer, pMsg, copySize);
                receiver->actualSize = copySize;
            }
            
            // Remove from list
            mailboxes[mboxId].waitingReceiversHead = receiver->pNextProcess;
            if (mailboxes[mboxId].waitingReceiversHead)
            { 
				mailboxes[mboxId].waitingReceiversHead->pPrevProcess = NULL;// update new head's prev pointer if there is a new head after removing the receiver
			}
            else
            {
				mailboxes[mboxId].waitingReceiversTail = NULL; // if no more waiting receivers, set tail to NULL
            }
			unblock(receiver->pid); // unblock the receiver process that was waiting for this message
            
            enableInterrupts();
            return 0;
        }
        
		if (!wait) // If no receiver waiting and sender is non-blocking, return -2 to indicate would block
        {
            enableInterrupts();
            return -2;
        }
            
		int pid = k_getpid(); // Get sender's process ID to store in waiting process struct
		WaitingProcessPtr wp = (WaitingProcessPtr)malloc(sizeof(struct waiting_process));// Allocate a waiting process struct to represent the blocked sender
		if (!wp)// Check if malloc succeeded
        {
            enableInterrupts();
            return -1;
        }
		wp->pid = pid; // Store sender's process ID in waiting process struct
		wp->msgBuffer = pMsg; // Store sender's message pointer in waiting process struct so it can be used to deliver the message when unblocked
		wp->msgSize = msg_size; // Store sender's message size in waiting process struct so it can be used to determine how much data to copy when delivering the message
		wp->actualSize = 0; // Initialize actualSize to 0; will be set to the size of the message actually delivered when unblocked
		wp->pNextProcess = NULL; // Initialize next pointer to NULL since this will be added to the end of the waiting senders list
		wp->pPrevProcess = NULL; // Initialize prev pointer to NULL since this will be added to the end of the waiting senders list
        
		if (!mailboxes[mboxId].waitingSendersTail) // If no senders currently waiting, set head and tail to this sender
        {
            mailboxes[mboxId].waitingSendersHead = wp;
            mailboxes[mboxId].waitingSendersTail = wp;
        }
		else // Otherwise, add to end of waiting senders list and update tail pointer
        {
            mailboxes[mboxId].waitingSendersTail->pNextProcess = wp;
            wp->pPrevProcess = mailboxes[mboxId].waitingSendersTail;
            mailboxes[mboxId].waitingSendersTail = wp;
        }
        
        enableInterrupts();
		block(BLOCKED_SEND); // Block the sender process until it can be unblocked by a receiver or if the mailbox is freed while waiting (in which case it should unblock with an exit code of -5)
        disableInterrupts();

        if (mailboxes[mboxId].status != MBSTATUS_INUSE)// If mailbox was freed while sender was waiting, unblock with exit code of -5
        {
            // Remove from list if still there
			if (wp->pPrevProcess)// If there is a previous sender in the list, update its next pointer to skip over this sender
            {

                wp->pPrevProcess->pNextProcess = wp->pNextProcess;
            }
			else if (mailboxes[mboxId].waitingSendersHead == wp)// If this sender is still the head of the waiting senders list, update head pointer
            {
                mailboxes[mboxId].waitingSendersHead = wp->pNextProcess;
            }
            if (wp->pNextProcess)
                wp->pNextProcess->pPrevProcess = wp->pPrevProcess;
            else if (mailboxes[mboxId].waitingSendersTail == wp)
                mailboxes[mboxId].waitingSendersTail = wp->pPrevProcess;

			free(wp); // Free the waiting process struct since the sender is no longer waiting
			enableInterrupts(); // Re-enable interrupts before exiting
            k_exit(-5); //Was blocked on zero-slot mailbox when freed - must exit with -5 
        }
        
        free(wp);
        enableInterrupts();
        return 0;
    }

    
    // If a receiver is waiting, deliver message directly (bypass slot)
    WaitingProcessPtr receiver = mailboxes[mboxId].waitingReceiversHead;
    if (receiver) 
    {
        // Copy message to receiver's buffer
		if (receiver->msgBuffer && pMsg) // check if the receiver's message buffer and the sender's message pointer are valid before copying
        {
            
			int copySize = (msg_size < receiver->msgSize) ? msg_size : receiver->msgSize;// determine how much data to copy based on sender's message size and receiver's buffer size
			memcpy(receiver->msgBuffer, pMsg, copySize); // copy the message directly to the receiver's buffer
			receiver->actualSize = copySize; // set the actualSize field in the waiting process struct to indicate how much data was delivered to the receiver (this is needed in case the sender's message size exceeds the receiver's buffer size, so the receiver knows how much of the message was actually delivered)
        }

		mailboxes[mboxId].waitingReceiversHead = receiver->pNextProcess; // Remove the receiver from the waiting receivers list by updating the head pointer to the next receiver in the list
		if (mailboxes[mboxId].waitingReceiversHead) // If there is a new head of the waiting receivers list after removing the receiver, update its prev pointer to NULL since it is now the head of the list
        {
			mailboxes[mboxId].waitingReceiversHead->pPrevProcess = NULL; // update new head's prev pointer if there is a new head after removing the receiver
        }
		else // If there are no more waiting receivers after removing this receiver, set the tail pointer to NULL as well
        {
			mailboxes[mboxId].waitingReceiversTail = NULL; // if no more waiting receivers, set tail to NULL
        }
           

		unblock(receiver->pid); // Unblock the receiver process that was waiting for this message so it can continue executing now that it has received the message

        enableInterrupts();
        return 0;
    }
    
    // Count slots
    int slotCount = 0;
	SlotPtr slot = mailboxes[mboxId].pSlotListHead; // start at the head of the slot list for this mailbox
	while (slot) // iterate through the slot list and count how many slots are currently in use for this mailbox
    {
		slotCount++; // increment slot count for each slot in the list
		slot = slot->pNextSlot; // move to the next slot in the list
    }
    
    // Block if mailbox is full
	while (slotCount >= mailboxes[mboxId].slotCount) // if the number of slots currently in use for this mailbox is greater than or equal to the total slot count for this mailbox, then the mailbox is full and the sender must block until a slot becomes available
    {
		if (!wait) // If sender is non-blocking, return -2 to indicate would block
        {
            enableInterrupts();
            return -2;
        }

        int pid = k_getpid();
		WaitingProcessPtr wp = (WaitingProcessPtr)malloc(sizeof(struct waiting_process)); // Allocate a waiting process struct to represent the blocked sender
		if (!wp) // Check if malloc succeeded
        {
            enableInterrupts();
            return -1;
        }
		wp->pid = pid; // Store sender's process ID in waiting process struct
		wp->msgBuffer = pMsg; // Store sender's message pointer in waiting process struct so it can be used to deliver the message when unblocked   
		wp->msgSize = msg_size;  // Store sender's message size in waiting process struct so it can be used to determine how much data to copy when delivering the message
        wp->actualSize = 0; // Initialize actual size to 0
		wp->pNextProcess = NULL; // Initialize next pointer to NULL since this will be added to the end of the waiting senders list
		wp->pPrevProcess = NULL; // Initialize prev pointer to NULL since this will be added to the end of the waiting senders list

		if (!mailboxes[mboxId].waitingSendersTail) // If no senders currently waiting, set head and tail to this sender
        {
			mailboxes[mboxId].waitingSendersHead = wp; // set head of waiting senders list 
			mailboxes[mboxId].waitingSendersTail = wp; // set tail of waiting senders list
        }
		else // Otherwise, add to end of waiting senders list and update tail pointer
        {
            mailboxes[mboxId].waitingSendersTail->pNextProcess = wp;
            wp->pPrevProcess = mailboxes[mboxId].waitingSendersTail;
            mailboxes[mboxId].waitingSendersTail = wp;
        }

        enableInterrupts();
        block(BLOCKED_SEND);
        disableInterrupts();

        if (mailboxes[mboxId].status != MBSTATUS_INUSE) {
            // Remove from list if still there
            if (wp->pPrevProcess)
            {
                wp->pPrevProcess->pNextProcess = wp->pNextProcess;
            }
            else if (mailboxes[mboxId].waitingSendersHead == wp)
            {
                mailboxes[mboxId].waitingSendersHead = wp->pNextProcess;
            }
            if (wp->pNextProcess)
            {
                wp->pNextProcess->pPrevProcess = wp->pPrevProcess;
            }
            else if (mailboxes[mboxId].waitingSendersTail == wp)
            {
                mailboxes[mboxId].waitingSendersTail = wp->pPrevProcess;
            }
            free(wp);
            enableInterrupts();
            k_exit(-5); /* was blocked on zero-slot mailbox when freed - must exit with -5 */
        }
        
		free(wp); // Free the waiting process struct since the sender is no longer waiting
        
        // Recount slots
		slotCount = 0; // reset slot count before recounting
		slot = mailboxes[mboxId].pSlotListHead; // start at the head of the slot list for this mailbox again to recount how many slots are currently in use after being unblocked (in case a slot became available while waiting)
        while (slot) {
            slotCount++;
            slot = slot->pNextSlot;
        }
    }
    
    // Create and add slot - check global pool first
    if (globalSlotsInUse >= MAXSLOTS)
    {
        // Global slot pool exhausted - halt the system
        console_output(FALSE, "No mail slots available.\n",
            globalSlotsInUse);
		enableInterrupts(); // Re-enable interrupts before halting
        stop(1); 
        return -1; //unreachable
    }

	SlotPtr newSlot = (SlotPtr)malloc(sizeof(MailSlot)); // Allocate a new mail slot to hold the message being sent
	if (!newSlot) // Check if malloc succeeded
    {
        enableInterrupts();
        return -1;
    }

    globalSlotsInUse++; /* consume one global slot */

	newSlot->mbox_id = mboxId; // set the mailbox ID for this slot to the ID of the mailbox it belongs to
	memcpy(newSlot->message, pMsg, msg_size); // copy the message data from the sender's message pointer into the slot's message buffer
	newSlot->messageSize = msg_size; // set the message size for this slot to the size of the message being sent (this is needed so that the receiver knows how much data is in the slot when it receives the message)
	newSlot->pNextSlot = NULL; // initialize next pointer to NULL since this will be added to the end of the slot list for the mailbox
	newSlot->pPrevSlot = NULL; // initialize prev pointer to NULL since this will be added to the end of the slot list for the mailbox

	if (!mailboxes[mboxId].pSlotListHead) // If no slots currently in mailbox, set head to this slot
    {
		mailboxes[mboxId].pSlotListHead = newSlot; // set head of slot list for this mailbox to the new slot
    }
    else
    {
		SlotPtr last = mailboxes[mboxId].pSlotListHead; // Otherwise, add to end of slot list for this mailbox
		while (last->pNextSlot) // iterate to the end of the slot list for this mailbox to find the last slot
			last = last->pNextSlot; // move to the next slot in the list until we reach the last slot (where next pointer is NULL)
		last->pNextSlot = newSlot; // set the next pointer of the last slot in the list to point to the new slot, effectively adding the new slot to the end of the list
		newSlot->pPrevSlot = last; // set the prev pointer of the new slot to point back to the last slot in the list for easier traversal in both directions if needed
    }

    enableInterrupts();
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
    disableInterrupts();
    
    // Validate mailbox id
    if (mboxId < 0 || mboxId >= MAXMBOX || mailboxes[mboxId].status != MBSTATUS_INUSE) {
        enableInterrupts();
        return -1;
    }

    // Zero-slot mailbox
    if (mailboxes[mboxId].slotCount == 0)
    {
        // If a sender is waiting, unblock and deliver
        WaitingProcessPtr sender = mailboxes[mboxId].waitingSendersHead;
		if (sender) // If there is a sender waiting, copy the message directly from the sender's buffer to the receiver's buffer and unblock the sender
        {
            // Copy message from sender's buffer
            int copySize = 0;
			if (sender->msgBuffer && pMsg) // check if the sender's message buffer and the receiver's message pointer are valid before copying
            {
				copySize = (sender->msgSize < msg_size) ? sender->msgSize : msg_size; // determine how much data to copy based on sender's message size and receiver's buffer size
				memcpy(pMsg, sender->msgBuffer, copySize); // copy the message directly from the sender's buffer to the receiver's buffer
            }
            
            // Remove from list
			mailboxes[mboxId].waitingSendersHead = sender->pNextProcess; // Remove the sender from the waiting senders list by updating the head pointer to the next sender in the list
			if (mailboxes[mboxId].waitingSendersHead) // If there is a new head of the waiting senders list after removing the sender, update its prev pointer to NULL since it is now the head of the list
            {
				mailboxes[mboxId].waitingSendersHead->pPrevProcess = NULL; // update new head's prev pointer if there is a new head after removing the sender
            }
            else
            {
				mailboxes[mboxId].waitingSendersTail = NULL; // If there are no more waiting senders after removing this sender, set the tail pointer to NULL as well
            }
			unblock(sender->pid); // Unblock the sender process that was waiting to send this message so it can continue executing now that its message has been received
            
            enableInterrupts();
            return copySize;
        }
        
        if (!wait)
        {
            enableInterrupts();
            return -2;
        }
            
        int pid = k_getpid();
        WaitingProcessPtr wp = (WaitingProcessPtr)malloc(sizeof(struct waiting_process));
		if (!wp) // Check if malloc succeeded
        {
            enableInterrupts();
            return -1;
        }
		wp->pid = pid; // Store receiver's process ID in waiting process struct
		wp->msgBuffer = pMsg; // Store receiver's message pointer in waiting process struct so it can be used to receive the message when unblocked
		wp->msgSize = msg_size; // Store receiver's buffer size in waiting process strucT
        wp->actualSize = 0; // Initialize actual size to 0
        wp->pNextProcess = NULL;
        wp->pPrevProcess = NULL;
        
        if (!mailboxes[mboxId].waitingReceiversTail) 
        {
			mailboxes[mboxId].waitingReceiversHead = wp;// If no receivers currently waiting, set head and tail to this receiver
			mailboxes[mboxId].waitingReceiversTail = wp;// set tail of waiting receivers list to this receiver
        }
		else // Otherwise, add to end of waiting receivers list and update tail pointer
        {
            mailboxes[mboxId].waitingReceiversTail->pNextProcess = wp;
            wp->pPrevProcess = mailboxes[mboxId].waitingReceiversTail;
            mailboxes[mboxId].waitingReceiversTail = wp;
        }
        
		enableInterrupts(); // Re-enable interrupts before blocking since the receiver is now added to the waiting receivers list and can be safely blocked without risking missing a wakeup
		block(BLOCKED_RECEIVE); // Block the receiver process until it can be unblocked by a sender delivering a message directly to it or if the mailbox is freed while waiting 
		disableInterrupts(); // Disable interrupts again after being unblocked to safely check conditions and access shared data structures
        
        if (mailboxes[mboxId].status != MBSTATUS_INUSE)
        {
            // Remove from list if still there
            if (wp->pPrevProcess)
            { 
				wp->pPrevProcess->pNextProcess = wp->pNextProcess; // If there is a previous receiver in the list, update its next pointer to skip over this receiver
            }
			else if (mailboxes[mboxId].waitingReceiversHead == wp) // If this receiver is still the head of the waiting receivers list, update head pointer
            {
				mailboxes[mboxId].waitingReceiversHead = wp->pNextProcess; // update head pointer to the next receiver in the list since this receiver is being removed
            }
            if (wp->pNextProcess)
            {
                wp->pNextProcess->pPrevProcess = wp->pPrevProcess; // If there is a next receiver in the list, update its previous pointer to skip over this receiver
            }
            else if (mailboxes[mboxId].waitingReceiversTail == wp) // If this receiver is still the tail of the waiting receivers list, update tail pointer
            {
                mailboxes[mboxId].waitingReceiversTail = wp->pPrevProcess; // update tail pointer to the previous receiver in the list since this receiver is being removed
            }

            free(wp);
            enableInterrupts();
			k_exit(-5); // exit the process if signaled while waiting, since receiver is blocked and can't return -5
        }
       
		int result = wp->actualSize; // store the actual size of the message delivered in a local variable 
        free(wp); 
        enableInterrupts();
        return result;
    }

    
	SlotPtr slot = mailboxes[mboxId].pSlotListHead; // Check if a slot is available; if not, block until one is freed by a receiver
	while (!slot)// If there are no slots currently in the mailbox, then the receiver must block until a sender frees a slot by delivering a message to a waiting receiver or if the mailbox is freed while waiting (in which case it should unblock with an exit code of -5)
    {
		if (!wait) // If receiver is non-blocking, return -2 to indicate would block
        {
            enableInterrupts();
            return -2;
        }
        
        int pid = k_getpid(); 
		WaitingProcessPtr wp = (WaitingProcessPtr)malloc(sizeof(struct waiting_process));// Allocate a waiting process struct to represent the blocked receiver
        if (!wp)
        {
            enableInterrupts();
            return -1;
        }
        wp->pid = pid;
        wp->msgBuffer = pMsg;
        wp->msgSize = msg_size;
        wp->actualSize = 0;
        wp->pNextProcess = NULL;
        wp->pPrevProcess = NULL;
        
		if (!mailboxes[mboxId].waitingReceiversTail) // If no receivers currently waiting, set head and tail to this receiver
        {
            mailboxes[mboxId].waitingReceiversHead = wp;
            mailboxes[mboxId].waitingReceiversTail = wp;
        }
		else // Otherwise, add to end of waiting receivers list and update tail pointer
        {
            mailboxes[mboxId].waitingReceiversTail->pNextProcess = wp;
            wp->pPrevProcess = mailboxes[mboxId].waitingReceiversTail;
            mailboxes[mboxId].waitingReceiversTail = wp;
        }
        
        enableInterrupts();
        block(BLOCKED_RECEIVE);
        disableInterrupts();
        
        // Check if signaled/released
        if (mailboxes[mboxId].status != MBSTATUS_INUSE) {
            // Remove from list if still there
			if (wp->pPrevProcess) // If there is a previous receiver in the list, update its next pointer to skip over this receiver
            {  
				wp->pPrevProcess->pNextProcess = wp->pNextProcess; // update previous receiver's next pointer to skip over this receiver since it is being removed from the list
            }
			else if (mailboxes[mboxId].waitingReceiversHead == wp) // If this receiver is still the head of the waiting receivers list, update head pointer
            {
                mailboxes[mboxId].waitingReceiversHead = wp->pNextProcess;
            }
            if (wp->pNextProcess) // If there is a next receiver in the list, update its previous pointer to skip over this receiver
            {   wp->pNextProcess->pPrevProcess = wp->pPrevProcess;
            }
            else if (mailboxes[mboxId].waitingReceiversTail == wp) // If this receiver is still the tail of the waiting receivers list, update tail pointer
            {
                mailboxes[mboxId].waitingReceiversTail = wp->pPrevProcess;
            }
            free(wp);
            enableInterrupts();
			return -5; // return -5 to indicate was signaled while waiting since the receiver is blocked and can't return -5
        }
        
        // Check if message was delivered directly by sender (actualSize set)
        if (wp->actualSize > 0) {
            int result = wp->actualSize;
            free(wp);
            enableInterrupts();
            return result;
        }
        
        // Otherwise, recheck for slot in mailbox
        free(wp);
        slot = mailboxes[mboxId].pSlotListHead;
    }
    
    // Copy message from slot
    int copySize = (slot->messageSize < msg_size) ? slot->messageSize : msg_size;
    memcpy(pMsg, slot->message, copySize);

    // Remove slot from list
    mailboxes[mboxId].pSlotListHead = slot->pNextSlot;
    if (mailboxes[mboxId].pSlotListHead)
        mailboxes[mboxId].pSlotListHead->pPrevSlot = NULL;

    free(slot);
    globalSlotsInUse--; //return one slot to the global pool 
    
    // Unblock one waiting sender since we freed a slot
    WaitingProcessPtr sender = mailboxes[mboxId].waitingSendersHead;
	if (sender) // If there is a sender waiting, unblock it now that a slot has been freed by the receiver taking the message from the slot
    {
        mailboxes[mboxId].waitingSendersHead = sender->pNextProcess;
		if (mailboxes[mboxId].waitingSendersHead) // If there is a new head of the waiting senders list after removing the sender, update its prev pointer to NULL since it is now the head of the list
        { 
            mailboxes[mboxId].waitingSendersHead->pPrevProcess = NULL;
        }
        else
        {
            mailboxes[mboxId].waitingSendersTail = NULL;
        }
        
        if (sender->msgBuffer != NULL && sender->msgSize > 0
			&& globalSlotsInUse < MAXSLOTS) // If the sender has a valid message and there are still global slots available, create a new slot
        {
			SlotPtr senderSlot = (SlotPtr)malloc(sizeof(MailSlot));// Allocate a new mail slot for the sender's message that we will add to the mailbox now that we have freed a slot by the receiver taking a message from a slot
            if (senderSlot)
            {
				int senderCopySize = (sender->msgSize < mailboxes[mboxId].slotSize) // determine how much data to copy from the sender's message
					? sender->msgSize : mailboxes[mboxId].slotSize; // set the copy size to the smaller of the sender's message size and the mailbox's slot size to ensure we don't exceed the slot's capacity 
				senderSlot->mbox_id = mboxId; // set the mailbox ID for this slot to the ID of the mailbox it belongs to
				memcpy(senderSlot->message, sender->msgBuffer, senderCopySize); // copy the message data from the sender's message buffer into the slot's message buffer
                senderSlot->messageSize = senderCopySize; // set the message size for this slot to the size of the copied message
				senderSlot->pNextSlot = NULL; // initialize next pointer to NULL since this will be added to the end of the slot list for the mailbox
                senderSlot->pPrevSlot = NULL; // initialize previous pointer to NULL since this will be added to the end of the slot list for the mailbox 
                globalSlotsInUse++;

                // Append to end of slot list to preserve FIFO order
                if (!mailboxes[mboxId].pSlotListHead)
                {
                    mailboxes[mboxId].pSlotListHead = senderSlot;
                }
				else // Otherwise, add to end of slot list for this mailbox
                {
                    SlotPtr last = mailboxes[mboxId].pSlotListHead;
                    while (last->pNextSlot) last = last->pNextSlot;
                    last->pNextSlot = senderSlot;
                    senderSlot->pPrevSlot = last;
                }
            }
        }

        
        enableInterrupts();

		unblock(sender->pid); // Unblock the sender process that was waiting to send a message so it can continue executing now that a slot has been freed and it has been added to the mailbox if it had a valid message to send

        dispatcher();

        return copySize;
    }

    enableInterrupts();
    return copySize;
}



void device_unblock(int deviceId) // Unblock the sender waiting on the device's mailbox when an interrupt occurs for that device
{
    // Check if the device ID is valid
    if (deviceId < 0 || deviceId >= THREADS_MAX_DEVICES) {
        return;
    }

    // Unblock the waiting sender for the device's mailbox
    int mboxId = devices[deviceId].deviceMbox;
    WaitingProcessPtr wp = mailboxes[mboxId].waitingSendersHead;
    if (wp) {
        unblock(wp->pid);
        mailboxes[mboxId].waitingSendersHead = wp->pNextProcess;
        if (mailboxes[mboxId].waitingSendersHead)
        {
            mailboxes[mboxId].waitingSendersHead->pPrevProcess = NULL;
		}

        else
        {
            mailboxes[mboxId].waitingSendersTail = NULL;
        }
        free(wp);
    }

	
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
    disableInterrupts();

    if (mboxId < 0 || mboxId >= MAXMBOX || mailboxes[mboxId].status != MBSTATUS_INUSE)
    {
        enableInterrupts();
        return -1;
    }

    mailboxes[mboxId].status = MBSTATUS_RELEASED;

    // Count blocked processes BEFORE unblocking
    int blockedCount = 0;
    WaitingProcessPtr wp = mailboxes[mboxId].waitingSendersHead;
    while (wp) { blockedCount++; wp = wp->pNextProcess; }
    wp = mailboxes[mboxId].waitingReceiversHead;
    while (wp) { blockedCount++; wp = wp->pNextProcess; }

    // If no blocked processes, just clean up and return
    if (blockedCount == 0) {
        enableInterrupts();
        // ...existing code for cleanup...
        disableInterrupts();
        mailboxes[mboxId].waitingSendersHead = mailboxes[mboxId].waitingSendersTail = NULL;
        mailboxes[mboxId].waitingReceiversHead = mailboxes[mboxId].waitingReceiversTail = NULL;
        SlotPtr slot = mailboxes[mboxId].pSlotListHead;
        while (slot) {
            SlotPtr next = slot->pNextSlot;
            free(slot);
            globalSlotsInUse--;
            slot = next;
        }
        mailboxes[mboxId].pSlotListHead = NULL;
        mailboxes[mboxId].mbox_id = -1;
        mailboxes[mboxId].slotCount = 0;
        mailboxes[mboxId].slotSize = 0;
        mailboxes[mboxId].type = MB_ZEROSLOT;
        mailboxes[mboxId].status = MBSTATUS_EMPTY;
        enableInterrupts();
        if (signaled())
            return -5;
        return 0;
    }

    // Unblock all blocked processes (senders and receivers) in FIFO order
    wp = mailboxes[mboxId].waitingSendersHead;
    while (wp) {
        unblock(wp->pid);
        wp = wp->pNextProcess;
    }
    wp = mailboxes[mboxId].waitingReceiversHead;
    while (wp) {
        unblock(wp->pid);
        wp = wp->pNextProcess;
    }

    enableInterrupts();

    // Block itself if any previously blocked process hasn't returned yet
    // Use a static counter to track how many have returned
    static int mailboxFree_unblocked = 0;
    static int mailboxFree_blockedCount = 0;
    static int mailboxFree_freerPid = -1;
    if (mailboxFree_freerPid == -1) {
        mailboxFree_freerPid = k_getpid();
        mailboxFree_blockedCount = blockedCount;
        mailboxFree_unblocked = 0;
    }

    // Block the freeing process if there are any blocked processes
    if (mailboxFree_blockedCount > 0) {
        block(0); // Block itself until all previously blocked processes return
    }

    // Cleanup after all blocked processes have returned
    disableInterrupts();
    mailboxes[mboxId].waitingSendersHead = mailboxes[mboxId].waitingSendersTail = NULL;
    mailboxes[mboxId].waitingReceiversHead = mailboxes[mboxId].waitingReceiversTail = NULL;
    SlotPtr slot = mailboxes[mboxId].pSlotListHead;
    while (slot) {
        SlotPtr next = slot->pNextSlot;
        free(slot);
        globalSlotsInUse--;
        slot = next;
    }
    mailboxes[mboxId].pSlotListHead = NULL;
    mailboxes[mboxId].mbox_id = -1;
    mailboxes[mboxId].slotCount = 0;
    mailboxes[mboxId].slotSize = 0;
    mailboxes[mboxId].type = MB_ZEROSLOT;
    mailboxes[mboxId].status = MBSTATUS_EMPTY;
    enableInterrupts();

    // If this is a process that was unblocked by mailbox_free, return -5 and increment counter
    if (signaled()) {
        mailboxFree_unblocked++;
        // If this is the last process, unblock the freeing process
        if (mailboxFree_unblocked == mailboxFree_blockedCount && mailboxFree_freerPid != -1) {
            unblock(mailboxFree_freerPid);
            mailboxFree_freerPid = -1;
            mailboxFree_blockedCount = 0;
            mailboxFree_unblocked = 0;
        }
        return -5;
    }

    // Reset static variables for next mailbox_free call
    mailboxFree_freerPid = -1;
    mailboxFree_blockedCount = 0;
    mailboxFree_unblocked = 0;
    return 0;
} /* mailbox_free */

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
	uint32_t deviceHandle = (uint32_t)-1; // Initialize to invalid handle value; will be set to correct handle if device name is valid
    uint32_t receivedStatus = 0;   // MUST be uint32_t to match what io_handler sends

    checkKernelMode("wait_device");

    if (status != NULL)
    {
        *status = 0;
    }

	if (strcmp(deviceName, "clock") == 0) // Special case for clock device since it doesn't have a standard device handle like disks and terminals; we can directly use the predefined clock device ID to identify it
    {
		deviceHandle = THREADS_CLOCK_DEVICE_ID; // Use predefined clock device ID as handle for clock device
    }
	else // For other devices, look up the device handle based on the device name using the device_handle function, which returns the corresponding device handle for valid device names or -1 for invalid names
    {
        deviceHandle = device_handle(deviceName);
    }

	if (deviceHandle >= 0 && deviceHandle < THREADS_MAX_DEVICES) // If the device handle is valid, proceed to block on the device's mailbox and wait for an interrupt to receive the status
    {
        disableInterrupts();
		waitingOnDevice++; // Increment global counter to indicate we are now waiting on a device; this is used by check_io_messaging to determine if there are any processes currently waiting on devices
        enableInterrupts();

        // Receive the full 32-bit status value from the device mailbox
        result = mailbox_receive(devices[deviceHandle].deviceMbox,
			&receivedStatus, sizeof(uint32_t), TRUE); // Block until we receive a message from the device's mailbox, which will contain the status value sent by the device's interrupt handler when the interrupt occurs

        disableInterrupts();
		waitingOnDevice--; // Decrement global counter since we are no longer waiting on the device after receiving the status message from the mailbox
        enableInterrupts();

        if (result >= 0)
        {
            if (status != NULL)
            {
                // Cast back to int for the caller - preserves all 32 bits
                *status = (int)receivedStatus;
            }
            result = 0;
        }
        else if (result == -5)
        {
            result = -5;  // Signaled
        }
    }
    else
    {
        console_output(FALSE, "Unknown device type: %s\n", deviceName);
        return -1;
    }

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

    // Register interrupt handlers in the handlers array
    handlers[THREADS_TIMER_INTERRUPT] = clock_interupt_handler;
    handlers[THREADS_SYS_CALL_INTERRUPT] = syscall_handler;

    // Register I/O interrupt handlers for all I/O devices (disks and terminals)
    handlers[THREADS_IO_INTERRUPT] = io_handler;
}

/*
 * Terminal interrupt handler: sends status to the correct terminal mailbox
 */
static void terminal_interrupt_handler(void* device, uint8_t command, uint32_t status, void* pArgs)
{
    // device is expected to be the device ID (int)
    int deviceId = (int)(intptr_t)device;
    if (deviceId < 0 || deviceId >= THREADS_MAX_DEVICES) {
        return;
    }
    int mbox = devices[deviceId].deviceMbox;
    int term_status = (int)status;
    mailbox_send(mbox, &term_status, sizeof(int), FALSE); // non-blocking send
}



 /* an error method to handle invalid syscalls */
static void nullsys(system_call_arguments_t* args)
{
    console_output(FALSE, "nullsys(): Invalid syscall %d. Halting...\n", args->call_id);
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

static void clock_interupt_handler(void* device, uint8_t command, uint32_t status, void* pArgs)
{
    static int count = 0;
    int clock_result = 0;
    uint32_t clockMbox;
	if (count == 4) // Fire the clock interrupt every 5th time since the timer may fire more frequently than we want to trigger the clock interrupt handler, so we can use a counter to only trigger the clock interrupt handler on every 5th timer interrupt (adjust as needed based on timer frequency and desired clock interrupt frequency)
    {
        /* get the clock mailbox */
        clockMbox = devices[THREADS_CLOCK_DEVICE_ID].deviceMbox;
        int clock_status = 0; // or use status if needed
        mailbox_send(clockMbox, &clock_status, sizeof(int), FALSE); // non-blocking send
        count = 0; // reset count after firing
    }
    else
    {
        count++;
    }
	time_slice(); // Call time_slice on every timer interrupt to ensure we are performing time slicing at the desired frequency
} /* clock_interrupt_handler */

static void syscall_handler(void* device, uint8_t command, uint32_t status, void* pArgs)
{
	LARGE_INTEGER unit; // Use LARGE_INTEGER to safely convert the void* device pointer to an integer device ID without losing information or causing issues on different platforms
	int deviceId; // Declare an integer variable to hold the device ID after conversion from the void* device pointer

	unit.QuadPart = (LONGLONG)device; // Convert the void* device pointer to a LARGE_INTEGER to safely handle the conversion to an integer device ID
	deviceId = unit.LowPart; // Extract the device ID from the LowPart of the LARGE_INTEGER after conversion

    /* verify the device id */
	if (deviceId != THREADS_SYSTEM_CALL_ID) // Check if the device ID from the interrupt matches the expected system call ID; 
    {
        
        console_output(FALSE, "syscall_handler(): invalid device id %d.\n", deviceId);
        stop(1);

        return;
    }

    if (pArgs != NULL)
    {
        /* note that the memory for this must be allocated or persistent. */
        system_call_arguments_t* pSysCallArgs = pArgs;

        /* Verify the system call identifier using pSysCallArgs->call_id */

        /* call the system call handler if one is registered. */
        if (pSysCallArgs->call_id >= 0 && pSysCallArgs->call_id < THREADS_MAX_SYSCALLS)
        {
            systemCallVector[pSysCallArgs->call_id](pSysCallArgs);
        }
        else
        {
            console_output(FALSE, "syscall_handler(): invalid system call id %d.\n", pSysCallArgs->call_id);
            stop(1);
        }

    }
} /* syscall_handler */

static void clock_handler_2(void* device, uint8_t command, uint32_t status, void* pArgs)
{
    static int count = 0;
    int clock_result = 0;
    int result = 0;
    uint32_t clockMbox;
    if (count == 4)
    {
        /* get the clock mailbox */
        clockMbox = devices[THREADS_CLOCK_DEVICE_ID].deviceMbox;
        int clock_status = 0; // or use status if needed
        result = mailbox_send(clockMbox, &clock_status, sizeof(int), FALSE); // non-blocking send
        if (result < 0) {
            console_output(FALSE, "Failed to send clock status to mailbox. Error code: %d\n", result);
        }
        count = 0; // reset count after firing          


        
    }
    else
    {
        count++;
    }
    time_slice();
} /* clock_handler */

static void io_handler(void* device, uint8_t command, uint32_t status, void* pArgs)
{
    LARGE_INTEGER unit;
    int result;
    int deviceId;

    unit.QuadPart = (LONGLONG)device; 
    deviceId = unit.LowPart;

    if (deviceId < 0 || deviceId >= THREADS_MAX_DEVICES)
    {
        console_output(FALSE, "io_handler(): device id out of range.\n");
        return;
    }

    
	result = mailbox_send(devices[deviceId].deviceMbox, &status, sizeof(uint32_t), FALSE); // non-blocking send of the status value to the device's mailbox so that any process waiting on that mailbox can receive the status and unblock when an interrupt occurs for that device
    if (result < 0)
    {
        console_output(FALSE, "Failed to send I/O status to mailbox for device %d. Error code: %d\n",
            deviceId, result);
    }
} /* io_handler */