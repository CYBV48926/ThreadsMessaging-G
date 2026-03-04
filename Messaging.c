
* ------------------------------------------------------------------------
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
static void syscall_handler(void* device, uint8_t command, uint32_t status, void *pArgs);

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
    for (int i = 0; i < THREADS_MAX_SYSCALLS; i++)
    {
        systemCallVector[i] = nullsys;
    }

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
    if (mboxId < 0 || mboxId >= MAXMBOX || mailboxes[mboxId].status != MBSTATUS_INUSE)
        return -1;
    if (msg_size > mailboxes[mboxId].slotSize)
        return -1;

    // Zero-slot mailbox: direct rendezvous (synchronization)
    if (mailboxes[mboxId].slotCount == 0) {
        // If a receiver is waiting, unblock and deliver
        WaitingProcessPtr receiver = mailboxes[mboxId].waitingReceiversHead;
        if (receiver) {
            unblock(receiver->pid);
            mailboxes[mboxId].waitingReceiversHead = receiver->pNextProcess;
            if (mailboxes[mboxId].waitingReceiversHead)
                mailboxes[mboxId].waitingReceiversHead->pPrevProcess = NULL;
            else
                mailboxes[mboxId].waitingReceiversTail = NULL;
            free(receiver);
            return 0;
        }
        // No receiver waiting: block or return -2
        if (!wait)
            return -2;
        int pid = k_getpid();
        WaitingProcessPtr wp = (WaitingProcessPtr)malloc(sizeof(struct waiting_process));
        wp->pid = pid;
        wp->pNextProcess = NULL;
        wp->pPrevProcess = NULL;
        if (!mailboxes[mboxId].waitingSendersTail) {
            mailboxes[mboxId].waitingSendersHead = wp;
            mailboxes[mboxId].waitingSendersTail = wp;
        } else {
            mailboxes[mboxId].waitingSendersTail->pNextProcess = wp;
            wp->pPrevProcess = mailboxes[mboxId].waitingSendersTail;
            mailboxes[mboxId].waitingSendersTail = wp;
        }
        block(BLOCKED_SEND);
        // After unblocking, check if mailbox is released
        if (mailboxes[mboxId].status != MBSTATUS_INUSE)
            return -5;
        return 0;
    }

    // Normal mailbox logic
    // If a receiver is waiting, deliver message directly (bypass slot)
    WaitingProcessPtr receiver = mailboxes[mboxId].waitingReceiversHead;
    if (receiver) {
        // Copy message to receiver's buffer (assume buffer pointer is stored in receiver struct if needed)
        unblock(receiver->pid);
        mailboxes[mboxId].waitingReceiversHead = receiver->pNextProcess;
        if (mailboxes[mboxId].waitingReceiversHead)
            mailboxes[mboxId].waitingReceiversHead->pPrevProcess = NULL;
        else
            mailboxes[mboxId].waitingReceiversTail = NULL;
        free(receiver);
        return 0;
    }
    int slotCount = 0;
    SlotPtr slot = mailboxes[mboxId].pSlotListHead;
    while (slot) {
        slotCount++;
        slot = slot->pNextSlot;
    }
    while (slotCount >= mailboxes[mboxId].slotCount) {
        if (!wait)
            return -2;
        int pid = k_getpid();
        WaitingProcessPtr wp = (WaitingProcessPtr)malloc(sizeof(struct waiting_process));
        wp->pid = pid;
        wp->pNextProcess = NULL;
        wp->pPrevProcess = NULL;
        disableInterrupts();
        if (mailboxes[mboxId].status != MBSTATUS_INUSE) {
            enableInterrupts();
            free(wp);
            return -5;
        }
        if (!mailboxes[mboxId].waitingSendersTail) {
            mailboxes[mboxId].waitingSendersHead = wp;
            mailboxes[mboxId].waitingSendersTail = wp;
        } else {
            mailboxes[mboxId].waitingSendersTail->pNextProcess = wp;
            wp->pPrevProcess = mailboxes[mboxId].waitingSendersTail;
            mailboxes[mboxId].waitingSendersTail = wp;
        }
        enableInterrupts();
        block(BLOCKED_SEND);
        slotCount = 0;
        slot = mailboxes[mboxId].pSlotListHead;
        while (slot) {
            slotCount++;
            slot = slot->pNextSlot;
        }
    }
    SlotPtr newSlot = (SlotPtr)malloc(sizeof(MailSlot));
    if (!newSlot)
        return -1;
    newSlot->mbox_id = mboxId;
    memcpy(newSlot->message, pMsg, msg_size);
    newSlot->messageSize = msg_size;
    newSlot->pNextSlot = NULL;
    newSlot->pPrevSlot = NULL;
    if (!mailboxes[mboxId].pSlotListHead) {
        mailboxes[mboxId].pSlotListHead = newSlot;
    } else {
        SlotPtr last = mailboxes[mboxId].pSlotListHead;
        while (last->pNextSlot)
            last = last->pNextSlot;
        last->pNextSlot = newSlot;
        newSlot->pPrevSlot = last;
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
    // Validate mailbox id
    if (mboxId < 0 || mboxId >= MAXMBOX || mailboxes[mboxId].status != MBSTATUS_INUSE)
        return -1;

    // Zero-slot mailbox: direct rendezvous (synchronization)
    if (mailboxes[mboxId].slotCount == 0) {
        // If a sender is waiting, unblock and deliver
        WaitingProcessPtr sender = mailboxes[mboxId].waitingSendersHead;
        if (sender) {
            unblock(sender->pid);
            mailboxes[mboxId].waitingSendersHead = sender->pNextProcess;
            if (mailboxes[mboxId].waitingSendersHead)
                mailboxes[mboxId].waitingSendersHead->pPrevProcess = NULL;
            else
                mailboxes[mboxId].waitingSendersTail = NULL;
            free(sender);
            return 0;
        }
        // No sender waiting: block or return -2
        if (!wait)
            return -2;
        int pid = k_getpid();
        WaitingProcessPtr wp = (WaitingProcessPtr)malloc(sizeof(struct waiting_process));
        wp->pid = pid;
        wp->pNextProcess = NULL;
        wp->pPrevProcess = NULL;
        if (!mailboxes[mboxId].waitingReceiversTail) {
            mailboxes[mboxId].waitingReceiversHead = wp;
            mailboxes[mboxId].waitingReceiversTail = wp;
        } else {
            mailboxes[mboxId].waitingReceiversTail->pNextProcess = wp;
            wp->pPrevProcess = mailboxes[mboxId].waitingReceiversTail;
            mailboxes[mboxId].waitingReceiversTail = wp;
        }
        block(BLOCKED_RECEIVE);
        // After unblocking, check if mailbox is released
        if (mailboxes[mboxId].status != MBSTATUS_INUSE)
            return -5;
        return 0;
    }

    // Normal mailbox logic
    SlotPtr slot = mailboxes[mboxId].pSlotListHead;
    while (!slot) {
        if (!wait)
            return -2;
        int pid = k_getpid();
        WaitingProcessPtr wp = (WaitingProcessPtr)malloc(sizeof(struct waiting_process));
        wp->pid = pid;
        wp->pNextProcess = NULL;
        wp->pPrevProcess = NULL;
        if (!mailboxes[mboxId].waitingReceiversTail) {
            mailboxes[mboxId].waitingReceiversHead = wp;
            mailboxes[mboxId].waitingReceiversTail = wp;
        } else {
            mailboxes[mboxId].waitingReceiversTail->pNextProcess = wp;
            wp->pPrevProcess = mailboxes[mboxId].waitingReceiversTail;
            mailboxes[mboxId].waitingReceiversTail = wp;
        }
        block(BLOCKED_RECEIVE);
        slot = mailboxes[mboxId].pSlotListHead;
    }
    int copySize = (slot->messageSize < msg_size) ? slot->messageSize : msg_size;
    memcpy(pMsg, slot->message, copySize);
    mailboxes[mboxId].pSlotListHead = slot->pNextSlot;
    if (mailboxes[mboxId].pSlotListHead)
        mailboxes[mboxId].pSlotListHead->pPrevSlot = NULL;
    free(slot);
    WaitingProcessPtr sender = mailboxes[mboxId].waitingSendersHead;
    if (sender) {
        unblock(sender->pid);
        mailboxes[mboxId].waitingSendersHead = sender->pNextProcess;
        if (mailboxes[mboxId].waitingSendersHead)
            mailboxes[mboxId].waitingSendersHead->pPrevProcess = NULL;
        else
            mailboxes[mboxId].waitingSendersTail = NULL;
        free(sender);
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
    if (mboxId < 0 || mboxId >= MAXMBOX || mailboxes[mboxId].status != MBSTATUS_INUSE) // checking if the mailbox id is valid and the mailbox is in use
    {
        return -1;
	}

	mailboxes[mboxId].status = MBSTATUS_RELEASED; // set the status of the mailbox to released to indicate that it is being freed

	// handle waiting senders
	WaitingProcessPtr wp = mailboxes[mboxId].waitingSendersHead;
	while (wp) {
		WaitingProcessPtr next = wp->pNextProcess;
		unblock(wp->pid);
		free(wp);
		wp = next;
	}
	mailboxes[mboxId].waitingSendersHead = mailboxes[mboxId].waitingSendersTail = NULL;

	// handle waiting receivers
	wp = mailboxes[mboxId].waitingReceiversHead;
	while (wp) {
		WaitingProcessPtr next = wp->pNextProcess;
		unblock(wp->pid);
		free(wp);
		wp = next;
	}
	mailboxes[mboxId].waitingReceiversHead = mailboxes[mboxId].waitingReceiversTail = NULL;

	// free slots
	SlotPtr slot = mailboxes[mboxId].pSlotListHead;
	while (slot) {
		SlotPtr next = slot->pNextSlot;
		free(slot);
		slot = next;
	}
	mailboxes[mboxId].pSlotListHead = NULL;

	// reset fields...
    mailboxes[mboxId].mbox_id = -1;
    mailboxes[mboxId].slotCount = 0;
    mailboxes[mboxId].slotSize = 0;
    mailboxes[mboxId].type = MB_ZEROSLOT;
    mailboxes[mboxId].pSlotListHead = NULL;
    /* spec says return -5 if signaled. */
    if (signaled())
    {
        return -5;
	}
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
	checkKernelMode("waitdevice"); // checking the PSR for kernel mode

    enableInterrupts();

	if (strcmp(deviceName, "clock") == 0) // if the device name is "clock", set the device handle to the predefined clock device id
    {
		deviceHandle = THREADS_CLOCK_DEVICE_ID; // set the device handle to the predefined clock device id
    }
    else
    {
		deviceHandle = device_handle(deviceName); // for other device names, call the device_handle function to get the corresponding device handle based on the provided device name string

    }

	if (deviceHandle >= 0 && deviceHandle < THREADS_MAX_DEVICES) // if the device handle is valid, block on the device's mailbox to wait for an interrupt and receive the device status
    {
        /* set a flag that there is a process waiting on a device. */
        waitingOnDevice++;
		mailbox_receive(devices[deviceHandle].deviceMbox, status, sizeof(int), TRUE); // block on the device's mailbox until the interrupt handler sends the device status to the mailbox, and store the received status in the provided status pointer

		disableInterrupts(); // disable interrupts while updating the waitingOnDevice flag to prevent

		waitingOnDevice--; // decrement the flag to indicate that the process is no longer waiting on a device after receiving the status from the device's mailbox
    }
    else
    {
		console_output(FALSE, "Unknown device type."); // if the device name is not recognized, output an error message and halt the process
        stop(-1);
    }

    
	if (signaled()) // if the process was signaled while waiting, set the result to -5 to indicate that it was interrupted by a signal
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
    handlers[THREADS_TIMER_INTERRUPT] = clock_interrupt_handler;
    handlers[THREADS_SYS_CALL_INTERRUPT] = syscall_handler;

    // Register terminal interrupt handlers for terminal devices
    // Assuming terminal device IDs are 1-4 (after clock at 0)
    for (int i = 1; i <= 4; ++i) {
        handlers[i] = terminal_interrupt_handler;
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
    if (count == 4)
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
    time_slice();
} /* clock_interrupt_handler */

static void syscall_handler(void* device, uint8_t command, uint32_t status, void *pArgs)
{
    LARGE_INTEGER unit;
    int deviceId;

    unit.QuadPart = (LONGLONG)device;
    deviceId = unit.LowPart;

    /* verify the device id */
    if (deviceId != THREADS_SYSTEM_CALL_ID)
    {
        //logging code to add
        return;
    }

if (pArgs != NULL)
    {
        /* note that the memory for this must be allocated or persistent. */
        system_call_arguments_t* pSysCallArgs = pArgs;

        /* Verify the system call identifier using pSysCallArgs->call_id */

        /* call the system call handler if one is registered. */

    }

} /* syscall_handler */
