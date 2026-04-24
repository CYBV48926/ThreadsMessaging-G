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

#define DISK_ARM_ALG   DISK_ARM_ALG_FCFS
#define MICROSECONDS_PER_SECOND 1000000
#define DISK_INFO 0x01

static TList sleeping_processes;
static int sleeping_processes_mutex;
static int ClockDriver(char*);
static int DiskDriver(char*);
static void sysCall4(system_call_arguments_t* args);
static int sleep_compare(void* a, void* b);

typedef struct
{
    TListNode listNode;
    int pid;
    int waitSem;
    unsigned long long wakeup_time;
} SleepingProcess;

typedef struct
{
    int platters;
    int sectors;
    int tracks;
    int disk;
} DiskInformation;

static DiskInformation diskInfo[THREADS_MAX_DISKS];
static int diskRequestSems[THREADS_MAX_DISKS];

int sys_sleep(int seconds);
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
    systemCallVector[SYS_DISKINFO] = sysCall4;

    /* Initialize sleep queue + mutex */
    TListInitialize(&sleeping_processes, offsetof(SleepingProcess, listNode), sleep_compare);
    sleeping_processes_mutex = k_semcreate(1);

    if (sleeping_processes_mutex < 0)
    {
        console_output(TRUE, "SystemCallsEntryPoint(): Can't create sleeping_processes_mutex\n");
        stop(1);
    }

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
        // Safe wakeup semaphore for each driver
        diskRequestSems[i] = k_semcreate(0);
        
        sprintf(buf, "%d", i);
        sprintf(name, "DiskDriver%d", i);
        diskPids[i] = k_spawn(name, DiskDriver, buf, THREADS_MIN_STACK_SIZE * 4, HIGHEST_PRIORITY);
        if (diskPids[i] < 0)
        {
            console_output(TRUE, "start3(): Can't create disk driver %d\n", i);
            stop(1);
        }
    }

    /* Create first user-level process and wait for it to finish */
    int devicesPid = sys_spawn("DevicesEntryPoint", DevicesEntryPoint, NULL, 8 * THREADS_MIN_STACK_SIZE, 3);
    
    // Use sys_wait to wait for the user space tests (DevicesTest01) to complete and terminate
    sys_wait(&status);

    // 1. Signal the active clock driver
    k_kill(clockPID, 15); 
    
    // 2. Kill and forcefully wake up the idle disk drivers
    for (i = 0; i < THREADS_MAX_DISKS; i++)
    {
        k_kill(diskPids[i], 15); 
        k_semv(diskRequestSems[i]); // Kick driver out of idle loop
    }

    // 3. Gracefully wait for all kernel drivers to exit and clean themselves up
    for (i = 0; i < 1 + THREADS_MAX_DISKS; i++)
    {
        k_wait(&status); 
    }

    return 0; // Natural clean termination
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

        k_semp(sleeping_processes_mutex);

        {
            unsigned long long current_time = system_clock();

            while (sleeping_processes.count > 0)
            {
                SleepingProcess* pHead = (SleepingProcess*)sleeping_processes.pHead;
                
                /* List is sorted, so if head is not ready, rest are also not ready */
                if (pHead->wakeup_time > current_time)
                {
                    break;
                }

                SleepingProcess* pWake = (SleepingProcess*)TListPopNode(&sleeping_processes);
                
                /* Wake up the process */
                k_semv(pWake->waitSem);
                free(pWake);
            }
        }

        k_semv(sleeping_processes_mutex);
    }

    return 0;
}

static int DiskDriver(char* arg)
{
    int unit = atoi(arg);
    int status;
    char devName[16];
    device_control_block_t devRequest;

    sprintf(devName, "disk%d", unit);
    set_psr(get_psr() | PSR_INTERRUPTS);

    /* Read the disk info asynchronously during startup */
    memset(&devRequest, 0, sizeof(devRequest));
    devRequest.command = DISK_INFO;
    
    // THREADS populates platters, tracks, and sizes natively into the provided structure space
    devRequest.output_data = &diskInfo[unit]; 
    device_control(devName, devRequest);
    wait_device(devName, &status);

    while (!signaled())
    {
        /* Idle explicitly awaiting a request; harmlessly broken by OS shutdown k_kill -> k_semv */
        k_semp(diskRequestSems[unit]);
        
        if (signaled()) 
        {
            break; 
        }
    }
    
    return 0;
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

static void sysCall4(system_call_arguments_t* args)
{
    switch (args->call_id)
    {
    case SYS_SLEEP:
        args->arguments[3] = (intptr_t)sys_sleep((int)args->arguments[0]);
        break;
    case SYS_DISKINFO:
    {
        int unit = (int)args->arguments[0];
        int* platters = (int*)args->arguments[1];
        int* sectors = (int*)args->arguments[2];
        int* tracks = (int*)args->arguments[3];
        int* disk = (int*)args->arguments[4];

        if (unit < 0 || unit >= THREADS_MAX_DISKS)
        {
            args->arguments[5] = (intptr_t)-1;
            break;
        }

        if (platters) *platters = diskInfo[unit].platters;
        if (sectors) *sectors = THREADS_DISK_SECTOR_COUNT; 
        if (tracks) *tracks = diskInfo[unit].tracks;
        if (disk) *disk = 0; 

        args->arguments[5] = (intptr_t)0;
        break;
    }
    case SYS_DISKREAD:
    case SYS_DISKWRITE:
        args->arguments[5] = (intptr_t)-1;
        break;
    default:
        args->arguments[3] = (intptr_t)-1;
        break;
    }
}

int sys_sleep(int seconds)
{
    SleepingProcess* pProcInfo;
    int waitSem;

    if (seconds < 0) return -1;
    if (seconds == 0) return 0;

    waitSem = k_semcreate(0);
    if (waitSem < 0) return -1;

    pProcInfo = (SleepingProcess*)malloc(sizeof(SleepingProcess));
    if (pProcInfo == NULL)
    {
        k_semfree(waitSem);
        return -1;
    }

    pProcInfo->pid = k_getpid();
    pProcInfo->waitSem = waitSem;
    pProcInfo->wakeup_time = system_clock() + ((unsigned long long)seconds * MICROSECONDS_PER_SECOND);

    k_semp(sleeping_processes_mutex);
    TListAddNodeInOrder(&sleeping_processes, pProcInfo);
    k_semv(sleeping_processes_mutex);

    // Block calling process
    k_semp(waitSem);
    
    // Clear out
    k_semfree(waitSem);

    return 0;
}

static int sleep_compare(void* a, void* b)
{
    SleepingProcess* proc_a = (SleepingProcess*)a;
    SleepingProcess* proc_b = (SleepingProcess*)b;

    /* REVERSED logic to place shortest wakeup at the head of TList */
    if (proc_a->wakeup_time < proc_b->wakeup_time) return 1;
    if (proc_a->wakeup_time > proc_b->wakeup_time) return -1;
    return 0;
}