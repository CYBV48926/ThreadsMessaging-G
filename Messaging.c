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


/* ------------------------------------------------------------------------
     Name - SchedulerEntryPoint
     Purpose - Initializes mailboxes and interrupt vector.
               Start the Messaging test process.
     Parameters - one, default arg passed by k_spawn that is not used here.
----------------------------------------------------------------------- */
int SchedulerEntryPoint(void* arg)
{
    uint32_t psr = get_psr();
    int kernelMode = (psr & PSR_KERNEL_MODE) != 0;
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

    // Initialize all mailbox table entries to empty
    for (int i = 0; i < MAXMBOX; ++i)
    {
        mailboxes[i].mbox_id = -1;
        mailboxes[i].slotCount = 0;
        mailboxes[i].slotSize = 0;
        mailboxes[i].status = MBSTATUS_EMPTY;
        mailboxes[i].type = MB_ZEROSLOT;
        mailboxes[i].pSlotListHead = NULL;
        mailboxes[i].waitingSendersHead = NULL;
        mailboxes[i].waitingSendersTail = NULL;
        mailboxes[i].waitingReceiversHead = NULL;
        mailboxes[i].waitingReceiversTail = NULL;
    }

    // Initialize all device entries
    for (int i = 0; i < THREADS_MAX_DEVICES; ++i)
    {
        devices[i].deviceHandle = NULL;
        devices[i].deviceMbox = -1;
        devices[i].deviceType = 0;
        devices[i].deviceName[0] = '\0';
    }

    // Clock device: zero-slot mailbox (synchronization rendezvous)
    devices[THREADS_CLOCK_DEVICE_ID].deviceMbox = mailbox_create(0, sizeof(int));
    strcpy_s(devices[THREADS_CLOCK_DEVICE_ID].deviceName,
        sizeof(devices[THREADS_CLOCK_DEVICE_ID].deviceName), "clock");

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
        devices[handle].deviceHandle = (void*)(intptr_t)handle;
        devices[handle].deviceType   = (i < 2) ? 0 : 1;  // 0=disk, 1=terminal
        strcpy_s(devices[handle].deviceName,
            sizeof(devices[handle].deviceName), deviceNames[i]);
        
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
            mailboxes[i].mbox_id = i;
            mailboxes[i].slotCount = slots;
            mailboxes[i].slotSize = slot_size;
            mailboxes[i].status = MBSTATUS_INUSE;
            mailboxes[i].type = (slots == 0) ? MB_ZEROSLOT : (slots == 1 ? MB_SINGLESLOT : MB_MULTISLOT);
            mailboxes[i].pSlotListHead = NULL;
            mailboxes[i].waitingSendersHead = NULL;
            mailboxes[i].waitingSendersTail = NULL;
            mailboxes[i].waitingReceiversHead = NULL;
            mailboxes[i].waitingReceiversTail = NULL;
            
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
    if (mboxId < 0 || mboxId >= MAXMBOX || mailboxes[mboxId].status != MBSTATUS_INUSE) {
        enableInterrupts();
        return -1;
    }
    if (msg_size > mailboxes[mboxId].slotSize) {
        enableInterrupts();
        return -1;
    }
    
    // Zero-slot mailbox
    if (mailboxes[mboxId].slotCount == 0) {
        // If a receiver is waiting, unblock and deliver
        WaitingProcessPtr receiver = mailboxes[mboxId].waitingReceiversHead;
        if (receiver) {
            // Deliver directly to waiting receiver
            if (receiver->msgBuffer && pMsg) {
                int copySize = (msg_size < receiver->msgSize) ? msg_size : receiver->msgSize;
                memcpy(receiver->msgBuffer, pMsg, copySize);
                receiver->actualSize = copySize;
            }
            
            // Remove from list
            mailboxes[mboxId].waitingReceiversHead = receiver->pNextProcess;
            if (mailboxes[mboxId].waitingReceiversHead)
                mailboxes[mboxId].waitingReceiversHead->pPrevProcess = NULL;
            else
                mailboxes[mboxId].waitingReceiversTail = NULL;
            
            unblock(receiver->pid);
            e
            enableInterrupts();
            return 0;
        }
        
        if (!wait) {
            enableInterrupts();
            return -2;
        }
            
        int pid = k_getpid();
        WaitingProcessPtr wp = (WaitingProcessPtr)malloc(sizeof(struct waiting_process));
        if (!wp) {
            enableInterrupts();
            return -1;
        }
        wp->pid = pid;
        wp->msgBuffer = pMsg;
        wp->msgSize = msg_size;
        wp->actualSize = 0;
        wp->pNextProcess = NULL;
        wp->pPrevProcess = NULL;
        
        if (!mailboxes[mboxId].waitingSendersTail) {
            mailboxes[mboxId].waitingSendersHead = wp;
            mailboxes[mboxId].waitingSendersTail = wp;
        }
        else {
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
                wp->pPrevProcess->pNextProcess = wp->pNextProcess;
            else if (mailboxes[mboxId].waitingSendersHead == wp)
                mailboxes[mboxId].waitingSendersHead = wp->pNextProcess;
                
            if (wp->pNextProcess)
                wp->pNextProcess->pPrevProcess = wp->pPrevProcess;
            else if (mailboxes[mboxId].waitingSendersTail == wp)
                mailboxes[mboxId].waitingSendersTail = wp->pPrevProcess;
            
            free(wp);
            enableInterrupts();
            return -5;
        }
        
        free(wp);
        enableInterrupts();
        return 0;
    }

    
    // If a receiver is waiting, deliver message directly (bypass slot)
    WaitingProcessPtr receiver = mailboxes[mboxId].waitingReceiversHead;
    if (receiver) {
        // Copy message to receiver's buffer
        if (receiver->msgBuffer && pMsg) {
            int copySize = (msg_size < receiver->msgSize) ? msg_size : receiver->msgSize;
            memcpy(receiver->msgBuffer, pMsg, copySize);
            receiver->actualSize = copySize;
        }
        
        mailboxes[mboxId].waitingReceiversHead = receiver->pNextProcess;
        if (mailboxes[mboxId].waitingReceiversHead)
            mailboxes[mboxId].waitingReceiversHead->pPrevProcess = NULL;
        else
            mailboxes[mboxId].waitingReceiversTail = NULL;
        
        unblock(receiver->pid);
        
        enableInterrupts();
        return 0;
    }
    
    // Count slots
    int slotCount = 0;
    SlotPtr slot = mailboxes[mboxId].pSlotListHead;
    while (slot) {
        slotCount++;
        slot = slot->pNextSlot;
    }
    
    // Block if mailbox is full
    while (slotCount >= mailboxes[mboxId].slotCount) {
        if (!wait) {
            enableInterrupts();
            return -2;
        }
        
        int pid = k_getpid();
        WaitingProcessPtr wp = (WaitingProcessPtr)malloc(sizeof(struct waiting_process));
        if (!wp) {
            enableInterrupts();
            return -1;
        }
        wp->pid = pid;
        wp->msgBuffer = NULL;
        wp->msgSize = 0;
        wp->actualSize = 0;
        wp->pNextProcess = NULL;
        wp->pPrevProcess = NULL;
        
        if (!mailboxes[mboxId].waitingSendersTail) {
            mailboxes[mboxId].waitingSendersHead = wp;
            mailboxes[mboxId].waitingSendersTail = wp;
        }
        else {
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
                wp->pPrevProcess->pNextProcess = wp->pNextProcess;
            else if (mailboxes[mboxId].waitingSendersHead == wp)
                mailboxes[mboxId].waitingSendersHead = wp->pNextProcess;
                
            if (wp->pNextProcess)
                wp->pNextProcess->pPrevProcess = wp->pPrevProcess;
            else if (mailboxes[mboxId].waitingSendersTail == wp)
                mailboxes[mboxId].waitingSendersTail = wp->pPrevProcess;
            
            free(wp);
            enableInterrupts();
            return -5;
        }
        
        free(wp);
        
        // Recount slots
        slotCount = 0;
        slot = mailboxes[mboxId].pSlotListHead;
        while (slot) {
            slotCount++;
            slot = slot->pNextSlot;
        }
    }
    
    // Create and add slot
    SlotPtr newSlot = (SlotPtr)malloc(sizeof(MailSlot));
    if (!newSlot) {
        enableInterrupts();
        return -1;
    }
    
    newSlot->mbox_id = mboxId;
    memcpy(newSlot->message, pMsg, msg_size);
    newSlot->messageSize = msg_size;
    newSlot->pNextSlot = NULL;
    newSlot->pPrevSlot = NULL;
    
    if (!mailboxes[mboxId].pSlotListHead) {
        mailboxes[mboxId].pSlotListHead = newSlot;
    }
    else {
        SlotPtr last = mailboxes[mboxId].pSlotListHead;
        while (last->pNextSlot)
            last = last->pNextSlot;
        last->pNextSlot = newSlot;
        newSlot->pPrevSlot = last;
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
    if (mailboxes[mboxId].slotCount == 0) {
        // If a sender is waiting, unblock and deliver
        WaitingProcessPtr sender = mailboxes[mboxId].waitingSendersHead;
        if (sender) {
            // Copy message from sender's buffer
            int copySize = 0;
            if (sender->msgBuffer && pMsg) {
                copySize = (sender->msgSize < msg_size) ? sender->msgSize : msg_size;
                memcpy(pMsg, sender->msgBuffer, copySize);
            }
            
            // Remove from list
            mailboxes[mboxId].waitingSendersHead = sender->pNextProcess;
            if (mailboxes[mboxId].waitingSendersHead)
                mailboxes[mboxId].waitingSendersHead->pPrevProcess = NULL;
            else
                mailboxes[mboxId].waitingSendersTail = NULL;
            
            unblock(sender->pid);
            
            enableInterrupts();
            return copySize;
        }
        
        if (!wait) {
            enableInterrupts();
            return -2;
        }
            
        int pid = k_getpid();
        WaitingProcessPtr wp = (WaitingProcessPtr)malloc(sizeof(struct waiting_process));
        if (!wp) {
            enableInterrupts();
            return -1;
        }
        wp->pid = pid;
        wp->msgBuffer = pMsg;
        wp->msgSize = msg_size;
        wp->actualSize = 0;
        wp->pNextProcess = NULL;
        wp->pPrevProcess = NULL;
        
        if (!mailboxes[mboxId].waitingReceiversTail) {
            mailboxes[mboxId].waitingReceiversHead = wp;
            mailboxes[mboxId].waitingReceiversTail = wp;
        }
        else {
            mailboxes[mboxId].waitingReceiversTail->pNextProcess = wp;
            wp->pPrevProcess = mailboxes[mboxId].waitingReceiversTail;
            mailboxes[mboxId].waitingReceiversTail = wp;
        }
        
        enableInterrupts();
        block(BLOCKED_RECEIVE);
        disableInterrupts();
        
        if (mailboxes[mboxId].status != MBSTATUS_INUSE) {
            // Remove from list if still there
            if (wp->pPrevProcess)
                wp->pPrevProcess->pNextProcess = wp->pNextProcess;
            else if (mailboxes[mboxId].waitingReceiversHead == wp)
                mailboxes[mboxId].waitingReceiversHead = wp->pNextProcess;
                
            if (wp->pNextProcess)
                wp->pNextProcess->pPrevProcess = wp->pPrevProcess;
            else if (mailboxes[mboxId].waitingReceiversTail == wp)
                mailboxes[mboxId].waitingReceiversTail = wp->pPrevProcess;
            
            free(wp);
            enableInterrupts();
			//return -5; attempting to return -5 here causes deadlock since the receiver is still blocked and can't return -5, so instead we will just exit the process with -5 as the exit code, which k_wait will see
			k_exit(-5); // exit the process if signaled while waiting, since receiver is blocked and can't return -5
        }
        
        // The message was already copied by the sender into pMsg
        // wp->actualSize contains the size
        int result = wp->actualSize;
        free(wp); 
        enableInterrupts();
        return result;
    }

    // Normal mailbox logic
    SlotPtr slot = mailboxes[mboxId].pSlotListHead;
    while (!slot) {
        if (!wait) {
            enableInterrupts();
            return -2;
        }
        
        int pid = k_getpid();
        WaitingProcessPtr wp = (WaitingProcessPtr)malloc(sizeof(struct waiting_process));
        if (!wp) {
            enableInterrupts();
            return -1;
        }
        wp->pid = pid;
        wp->msgBuffer = pMsg;
        wp->msgSize = msg_size;
        wp->actualSize = 0;
        wp->pNextProcess = NULL;
        wp->pPrevProcess = NULL;
        
        if (!mailboxes[mboxId].waitingReceiversTail) {
            mailboxes[mboxId].waitingReceiversHead = wp;
            mailboxes[mboxId].waitingReceiversTail = wp;
        }
        else {
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
            if (wp->pPrevProcess)
                wp->pPrevProcess->pNextProcess = wp->pNextProcess;
            else if (mailboxes[mboxId].waitingReceiversHead == wp)
                mailboxes[mboxId].waitingReceiversHead = wp->pNextProcess;
                
            if (wp->pNextProcess)
                wp->pNextProcess->pPrevProcess = wp->pPrevProcess;
            else if (mailboxes[mboxId].waitingReceiversTail == wp)
                mailboxes[mboxId].waitingReceiversTail = wp->pPrevProcess;
            
            free(wp);
            enableInterrupts();
            return -5;
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
    
    // Unblock one waiting sender since we freed a slot
    WaitingProcessPtr sender = mailboxes[mboxId].waitingSendersHead;
    if (sender) {
        mailboxes[mboxId].waitingSendersHead = sender->pNextProcess;
        if (mailboxes[mboxId].waitingSendersHead)
            mailboxes[mboxId].waitingSendersHead->pPrevProcess = NULL;
        else
            mailboxes[mboxId].waitingSendersTail = NULL;
        
        // MUST enable interrupts BEFORE calling unblock and dispatcher
        enableInterrupts();
        
        unblock(sender->pid);
        
        dispatcher();
        
        // Return the received message
        return copySize;
    }
    
    enableInterrupts();
    return copySize;
}



void device_unblock(int deviceId)
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
            mailboxes[mboxId].waitingSendersHead->pPrevProcess = NULL;
        else
            mailboxes[mboxId].waitingSendersTail = NULL;
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

   
    WaitingProcessPtr wp = mailboxes[mboxId].waitingSendersHead;
    while (wp) {
        WaitingProcessPtr next = wp->pNextProcess;
        unblock(wp->pid);
        wp = next;
    }

    // Unblock all waiting receivers  
    wp = mailboxes[mboxId].waitingReceiversHead;
    while (wp) {
        WaitingProcessPtr next = wp->pNextProcess;
        unblock(wp->pid);
        wp = next;
    }

    

    enableInterrupts();
    
    
    dispatcher();
    
    // Re-disable interrupts for cleanup
    disableInterrupts();
    
    
	mailboxes[mboxId].waitingSendersHead = mailboxes[mboxId].waitingSendersTail = NULL; // Clear sender lists
	mailboxes[mboxId].waitingReceiversHead = mailboxes[mboxId].waitingReceiversTail = NULL; // Clear receiver lists

    
    SlotPtr slot = mailboxes[mboxId].pSlotListHead;
    while (slot) {
        SlotPtr next = slot->pNextSlot;
        free(slot);
        slot = next;
    }
    mailboxes[mboxId].pSlotListHead = NULL;

    // Reset mailbox fields
    mailboxes[mboxId].mbox_id = -1;
    mailboxes[mboxId].slotCount = 0;
    mailboxes[mboxId].slotSize = 0;
    mailboxes[mboxId].type = MB_ZEROSLOT;
    
    // Set status to EMPTY so the mailbox can be reused
    mailboxes[mboxId].status = MBSTATUS_EMPTY;
    
    enableInterrupts();
    
    // Check if current process was signaled during cleanup
    if (signaled())
    {
        return -5;
    }
    
    return 0;
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
    uint32_t deviceHandle = (uint32_t)-1;
    uint32_t receivedStatus = 0;   // MUST be uint32_t to match what io_handler sends

    checkKernelMode("wait_device");

    if (status != NULL)
    {
        *status = 0;
    }

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
        disableInterrupts();
        waitingOnDevice++;
        enableInterrupts();

        // Receive the full 32-bit status value from the device mailbox
        result = mailbox_receive(devices[deviceHandle].deviceMbox,
                                 &receivedStatus, sizeof(uint32_t), TRUE);

        disableInterrupts();
        waitingOnDevice--;
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

static void syscall_handler(void* device, uint8_t command, uint32_t status, void* pArgs)
{
    LARGE_INTEGER unit;
    int deviceId;

    unit.QuadPart = (LONGLONG)device;
    deviceId = unit.LowPart;

    /* verify the device id */
    if (deviceId != THREADS_SYSTEM_CALL_ID)
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

    
    result = mailbox_send(devices[deviceId].deviceMbox, &status, sizeof(uint32_t), FALSE);
    if (result < 0)
    {
        console_output(FALSE, "Failed to send I/O status to mailbox for device %d. Error code: %d\n",
            deviceId, result);
    }
} /* io_handler */