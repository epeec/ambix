#ifndef _PNP_H
#define _PNP_H

#define MAX_PIDS 5
#define MAX_PID_N 2147483647 // set to INT_MAX. true max pid number is shown in /proc/sys/kernel/pid_max

// Find-related constants:
#define DRAM_MODE 0
#define NVRAM_MODE 1
#define SWITCH_MODE 2
#define MAX_N_FIND MAX_N_PER_PACKET * MAX_PACKETS - 1 // Amount of pages that fit in exactly MAX_PACKETS netlink packets making space for retval struct (end struct)
#define MAX_N_SWITCH (MAX_N_FIND - 1) / 2 // Amount of switches that fit in exactly MAX_PACKETS netlink packets making space for begin and end struct


// Node definition: DRAM nodes' ids must always be a lower value than NVRAM nodes' ids due to the memory policy set in client-placement.c
static const int DRAM_NODES[]  = {0,1};
static const int NVRAM_NODES[]  = {2,3};

static const int n_dram_nodes = sizeof(DRAM_NODES)/sizeof(DRAM_NODES[0]);
static const int n_nvram_nodes = sizeof(NVRAM_NODES)/sizeof(NVRAM_NODES[0]);

// Netlink:
#define NETLINK_USER 31
#define MAX_PAYLOAD 4096 // Theoretical max is 32KB - netlink header - padding but limiting payload to 4096 or page size is standard in kernel programming
#define MAX_PACKETS 512
#define MAX_N_PER_PACKET (MAX_PAYLOAD/sizeof(addr_info_t)) // Currently 1MB of pages


// Unix Domain Socket:
#define UDS_path "./socket"
#define MAX_BACKLOG 5

// Comm-related OP codes:
#define FIND_OP 0
#define BIND_OP 1
#define UNBIND_OP 2

// Comm-related structures:
typedef struct addr_info {
    unsigned long addr;
    int pid_retval; // Stores pid info for FIND operation and BIND/UNBIND ok/nok
} addr_info_t;

typedef struct req {
    int op_code;
    int pid_n; // Stores pid for BIND/UNBIND and the number of pages for FIND
    int mode;
} req_t;

//Client-ctl comms:
#define PORT 8080
#define SELECT_TIMEOUT 1

// Misc:
#define MAX_COMMAND_SIZE 80
#define DRAM_TARGET 0.80
#define DRAM_THRESH_PLUS 0.05
#define DRAM_THRESH_NEGATIVE 0.15
#define MEMCHECK_INTERVAL 2
#define SWITCH_INTERVAL 5


// Memory ranges: (64-bit systems only use 48-bit)
#define IS_64BIT (sizeof(void*) == 8)
#define MAX_ADDRESS (IS_64BIT ? 0xFFFF880000000000UL : 0xC0000000UL) // Max user-space addresses for the x86 architecture
#define MAX_ADDRESS_ARM (IS_64BIT ? 0x800000000000UL : 0xC0000000UL) // Max user-space addresses for the ARM architecture


// Helper function:

int contains(int value, int mode) {
    const int *array;
    int size, i;
    
    if(mode == NVRAM_MODE) {
        array = NVRAM_NODES;
        size = n_nvram_nodes;
    }
    else {
        array = DRAM_NODES;
        size = n_dram_nodes;
    }
    for(i=0; i<size; i++) {
        if(array[i] == value) {
            return 1;
        }
    }
    return 0;
}
#endif