#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <THREADSLib.h>
#include <Messaging.h>
#include <Scheduler.h>
#include <TList.h>
#include <libuser.h>
#include <SystemCalls.h>
#include <Devices.h>

/* Set the disk arm scheduling algorithm.
 * See Devices.h for available constants (DISK_ARM_ALG_FCFS, DISK_ARM_ALG_SSTF, etc.).
 * You must implement FCFS and SSTF. Change this value to test each algorithm.
 * Submissions will be assessed with DISK_ARM_ALG_FCFS and DISK_ARM_ALG_SSTF. */
#define DISK_ARM_ALG   DISK_ARM_ALG_FCFS

#define MICROSECONDS_PER_SECOND 1000000
#define DISK_INFO 0x01

static int ClockDriver(char*);
static int DiskDriver(char*);
static int sys_sleep(int seconds);
static int sys_disk_read(int unit, int track, int platter, int sector, char* buffer);
static int sys_disk_write(int unit, int track, int platter, int sector, char* buffer);
int wait_device(const char* deviceName, int* status);
static void disableInterrupts();
static void sysCall4(system_call_arguments_t* args);


typedef struct devices_proc
{
    struct devices_proc* pNext;
    struct devices_proc* pPrev;
    int pid;
} DevicesProcess;

typedef struct
{
    int tracks;
    int platters;
    char deviceName[THREADS_MAX_DEVICE_NAME];
} DiskInformation;

typedef struct
{
    int pid;
    long long wakeup_time;
} SleepingProcess;

typedef struct
{
    int pid;
    int operation; // 0 for read, 1 for write
    int track;
    int platter;
    int sector;
    char* buffer;
} DiskRequest;


static DevicesProcess devicesProcs[MAXPROC];
static DiskInformation diskInfo[THREADS_MAX_DISKS];
static TList sleeping_processes;
static int sleeping_processes_mutex;
static int disk_mailboxes[THREADS_MAX_DISKS];
//extern device_t devices[THREADS_MAX_DEVICES];
extern struct device_t
{
    char name[THREADS_MAX_DEVICE_NAME];
    int deviceMbox;
    // Add other fields as needed by your project
} devices[THREADS_MAX_DEVICES];
extern volatile int waitingOnDevice;




static inline void checkKernelMode(const char* functionName);
extern int DevicesEntryPoint(char*);

int SystemCallsEntryPoint(char* arg)
{
    char    buf[25];
    char    name[128];
    int     i;
    int     clockPID = 0;
    int     diskPids[THREADS_MAX_DISKS];
    int     status;

    checkKernelMode(__func__);

    /* Assign system call handlers */
    systemCallVector[SYS_SLEEP] = sysCall4;
    systemCallVector[SYS_DISKREAD] = sysCall4;
    systemCallVector[SYS_DISKWRITE] = sysCall4;


    /* Initialize the process table */
    for (int i = 0; i < MAXPROC; ++i)
    {
    }

    list_init(&sleeping_processes);
    sleeping_processes_mutex = k_semcreate(1);


    /* Create and start the clock driver */
    clockPID = k_spawn("Clock driver", ClockDriver, NULL, THREADS_MIN_STACK_SIZE, HIGHEST_PRIORITY);
    if (clockPID < 0)
    {
        console_output(TRUE, "start3(): Can't create clock driver\n");
        stop(1);
    }

    /* Create the disk drivers */
    for (i = 0; i < THREADS_MAX_DISKS; i++)
    {
        sprintf(buf, "%d", i);
        sprintf(name, "DiskDriver%d", i);
        disk_mailboxes[i] = k_mbxcreate(MAXPROC);
        diskPids[i] = k_spawn(name, DiskDriver, buf, THREADS_MIN_STACK_SIZE * 4, HIGHEST_PRIORITY);
        if (diskPids[i] < 0)
        {
            console_output(TRUE, "start3(): Can't create disk driver %d\n", i);
            stop(1);
        }
    }

    /* Create first user-level process and wait for it to finish */
    sys_spawn("DevicesEntryPoint", DevicesEntryPoint, NULL, 8 * THREADS_MIN_STACK_SIZE, 3);
    sys_wait(&status);

    /* Terminate the drivers */
    k_kill(clockPID, 15);
    k_wait(&status);

    for (i = 0; i < 2; i++)
    {
        k_kill(diskPids[i], 15);
        k_wait(&status);
    }

    return 0;
}

static int sleep_compare(const void* a, const void* b)
{
    SleepingProcess* proc_a = (SleepingProcess*)a;
    SleepingProcess* proc_b = (SleepingProcess*)b;
    if (proc_a->wakeup_time < proc_b->wakeup_time) return -1;
    if (proc_a->wakeup_time > proc_b->wakeup_time) return 1;
    return 0;
}

static int sys_sleep(int seconds)
{
    if (seconds < 0) return -1;

    SleepingProcess* proc_info = (SleepingProcess*)malloc(sizeof(SleepingProcess));
    if (!proc_info) return -1;

    proc_info->pid = get_pid();
    proc_info->wakeup_time = system_clock() + (long long)seconds * MICROSECONDS_PER_SECOND;

    k_semwait(sleeping_processes_mutex);
    list_insert_sorted(&sleeping_processes, proc_info, sleep_compare);
    k_semsignal(sleeping_processes_mutex);

    k_semp(0);

    /* The proc_info is now freed in the ClockDriver after the process is unblocked */
    return 0;
}

static int sys_disk_read(int unit, int track, int platter, int sector, char* buffer)
{
    DiskRequest request;
    request.pid = get_pid();
    request.operation = 0; // Read
    request.track = track;
    request.platter = platter;
    request.sector = sector;
    request.buffer = buffer;

    k_send(disk_mailboxes[unit], &request, sizeof(DiskRequest), 0);
    k_semp(0); // Block until I/O is complete

    return 0;
}

static int sys_disk_write(int unit, int track, int platter, int sector, char* buffer)
{
    DiskRequest request;
    request.pid = get_pid();
    request.operation = 1; // Write
    request.track = track;
    request.platter = platter;
    request.sector = sector;
    request.buffer = buffer;

    k_send(disk_mailboxes[unit], &request, sizeof(DiskRequest), 0);
    k_semp(0); // Block until I/O is complete

    return 0;
}



static int ClockDriver(char* arg)
{
    int result;
    int status;

    set_psr(get_psr() | PSR_INTERRUPTS);

    while (!signaled())
    {
        result = wait_device("clock", &status);
        if (result != 0)
        {
            return 0;
        }

        /* Compute the current time and wake up any processes whose time has come */
        k_semwait(sleeping_processes_mutex);
        long long current_time = system_clock();
        SleepingProcess* proc_info;

        while ((proc_info = (SleepingProcess*)list_peek_front(&sleeping_processes)) != NULL && proc_info->wakeup_time <= current_time)
        {
            list_remove_front(&sleeping_processes);
            k_unblock(proc_info->pid);
            free(proc_info); /* Free the structure after unblocking the process */
        }
        k_semsignal(sleeping_processes_mutex);
    }
    return 0;
}


typedef struct
{
    int tracks;
    int platters;
    char name[THREADS_MAX_DEVICE_NAME];
} DiskInfoResult;

static int DiskDriver(char* arg)
{
    int unit = atoi(arg);
    int currentTrack = 0;
    device_control_block_t devRequest;
    TList request_queue;
    list_init(&request_queue);

    set_psr(get_psr() | PSR_INTERRUPTS);

    /* Read the disk info */
    DiskInfoResult diskResult;
    devRequest.command = DISK_INFO;
    devRequest.control1 = (uint8_t)unit;
    devRequest.output_data = &diskResult;
    devRequest.data_length = sizeof(DiskInfoResult);
    device_io(&devRequest);
    diskInfo[unit].tracks = diskResult.tracks;
    diskInfo[unit].platters = diskResult.platters;
    strcpy(diskInfo[unit].deviceName, diskResult.name);


    /* Operating loop */
    while (!signaled())
    {
        DiskRequest* req;
        // Drain the mailbox of all requests
        while (1)
        {
            req = (DiskRequest*)malloc(sizeof(DiskRequest));
            if (k_recv(disk_mailboxes[unit], req, sizeof(DiskRequest), 1) != 0)
            {
                free(req);
                break;
            }
            list_append(&request_queue, req);
        }

        if (list_is_empty(&request_queue))
        {
            // Block until a new request arrives
            req = (DiskRequest*)malloc(sizeof(DiskRequest));
            if (k_recv(disk_mailboxes[unit], req, sizeof(DiskRequest), 0) == 0)
            {
                list_append(&request_queue, req);
            }
            else
            {
                free(req);
                continue; // Check signaled() again
            }
        }

        DiskRequest* next_req = NULL;
#if DISK_ARM_ALG == DISK_ARM_ALG_FCFS
        next_req = (DiskRequest*)list_remove_front(&request_queue);
#elif DISK_ARM_ALG == DISK_ARM_ALG_SSTF
        // Find the request with the shortest seek time
        TLink* current_link = request_queue.pHead;
        TLink* sstf_link = current_link;
        int min_seek;

        if (current_link != NULL)
        {
            min_seek = abs(((DiskRequest*)current_link->pData)->track - currentTrack);
            while (current_link != NULL)
            {
                int seek = abs(((DiskRequest*)current_link->pData)->track - currentTrack);
                if (seek < min_seek)
                {
                    min_seek = seek;
                    sstf_link = current_link;
                }
                current_link = current_link->pNext;
            }
            next_req = (DiskRequest*)list_remove(&request_queue, sstf_link);
        }
#endif

        if (next_req != NULL)
        {
            // Seek to the correct track
            devRequest.command = DISK_SEEK;
            devRequest.control2 = (uint8_t)next_req->track;
            device_io(&devRequest);
            currentTrack = next_req->track;

            // Perform read/write
            devRequest.command = (next_req->operation == 0) ? DISK_READ : DISK_WRITE;
            devRequest.control1 = (uint8_t)next_req->platter;
            devRequest.control2 = (uint8_t)next_req->sector;
            devRequest.input_data = (next_req->operation == 0) ? NULL : next_req->buffer;
            devRequest.output_data = (next_req->operation == 0) ? next_req->buffer : NULL;
            devRequest.data_length = THREADS_DISK_SECTOR_SIZE;
            device_io(&devRequest);

            k_unblock(next_req->pid);
            free(next_req);
        }
    }
    return 0;
}

int wait_device(const char* deviceName, int* status)
{
    int deviceHandle = -1;
    for (int i = 0; i < THREADS_MAX_DEVICES; i++)
    {
        if (strcmp(devices[i].name, deviceName) == 0)
        {
            deviceHandle = i;
            break;
        }
    }

    if (deviceHandle >= 0 && deviceHandle < THREADS_MAX_DEVICES)
    {
        /* set a flag that there is a process waiting on a device. */
        waitingOnDevice++;
        mailbox_receive(devices[deviceHandle].deviceMbox, status, sizeof(int), TRUE);
        disableInterrupts();
        waitingOnDevice--;
        return 0;
    }
    return -1; // Device not found
}



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

/*****************************************************************************
   Name - checkKernelMode
   Purpose - Checks the PSR for kernel mode and stops if in user mode
   Parameters -
   Returns -
   Side Effects - Will stop if not in kernel mode
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

static void disableInterrupts()
{
    union psr_values psr;
    psr.integer_part = get_psr();
    psr.bits.cur_int_enable = 0;
    set_psr(psr.integer_part);
}

static void sysCall4(system_call_arguments_t* args)
{
    switch (args->call_id)
    {
    case SYS_SLEEP:
        args->arguments[3] = (intptr_t)sys_sleep((int)args->arguments[0]);
        break;
    case SYS_DISKREAD:
        args->arguments[5] = (intptr_t)sys_disk_read((int)args->arguments[0], (int)args->arguments[1], (int)args->arguments[2], (int)args->arguments[3], (char*)args->arguments[4]);
        break;
    case SYS_DISKWRITE:
        args->arguments[5] = (intptr_t)sys_disk_write((int)args->arguments[0], (int)args->arguments[1], (int)args->arguments[2], (int)args->arguments[3], (char*)args->arguments[4]);
        break;
        //more code
    }
}
