/**
 * @file    placement.c
 * @author  Miguel Marques <miguel.soares.marques@tecnico.ulisboa.pt>
 * @date    12 March 2020
 * @version 0.3
 * @brief  Page walker for finding page table entries' R/M bits. Intended for the 5.6.3 Linux kernel.
 * Adapted from the code provided by Reza Karimi <r68karimi@gmail.com>
 * @see https://github.com/miguelmarques1904/ambix for a full description of the module.
 */

#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"

#include <linux/delay.h>
#include <linux/init.h>  // Macros used to mark up functions e.g., __init __exit
#include <linux/kernel.h>  // Contains types, macros, functions for the kernel
#include <linux/kthread.h>
#include <linux/mempolicy.h>
#include <linux/module.h>  // Core header for loading LKMs into the kernel
#include <net/sock.h>
#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <linux/mount.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/seq_file.h>
#include <linux/shmem_fs.h>
#include <linux/signal.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <linux/pagewalk.h>
#include <linux/mmzone.h> // Contains conversion between pfn and node id (NUMA node)

#include <linux/string.h>
#include "ambix.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Miguel Marques");
MODULE_DESCRIPTION("Bandwidth-aware page replacement");
MODULE_VERSION("1.11");
MODULE_INFO(vermagic, "5.8.5-patched SMP mod_unload modversions ");

struct sock *nl_sock;

addr_info_t *found_addrs;
addr_info_t *backup_addrs; // prevents a second page walk
addr_info_t *switch_backup_addrs; // for switch walk

struct task_struct **task_items;
struct nlmsghdr **nlmh_array;
int n_pids = 0;

volatile unsigned long last_addr_dram = 0;
volatile unsigned long last_addr_nvram = 0;

int last_pid_dram = 0;
int last_pid_nvram = 0;

int curr_pid = 0;
int n_to_find = 0;
int n_found = 0;
int n_backup = 0;
int n_switch_backup = 0;



/*
-------------------------------------------------------------------------------

HELPER FUNCTIONS

-------------------------------------------------------------------------------
*/



static int find_target_process(pid_t pid) {  // to find the task struct by process_name or pid
    if (n_pids >= MAX_PIDS) {
        pr_info("PLACEMENT: Managed PIDs at capacity.\n");
        return 0;
    }
    int i;
    for (i=0; i < n_pids; i++) {
        if ((task_items[i] != NULL) && (task_items[i]->pid == pid)) {
            pr_info("PLACEMENT: Already managing given PID.\n");
            return 0;
        }
    }

    struct pid *pid_s = find_get_pid(pid);
    if (pid_s == NULL) {
        return 0;
    }
    struct task_struct *t = get_pid_task(pid_s, PIDTYPE_PID);
    if (t != NULL) {
        task_items[n_pids++] = t;
        return 1;
    }

    return 0;
}

static int update_pid_list(int i) {
    if (last_pid_dram > i) {
        last_pid_dram--;
    }
    else if (last_pid_dram == i) {
        last_addr_dram = 0;

        if (last_pid_dram == (n_pids-1)) {
            last_pid_dram = 0;
        }
    }

    if (last_pid_nvram > i) {
        last_pid_nvram--;
    }
    else if (last_pid_nvram == i) {
        last_addr_nvram = 0;

        if (last_pid_nvram == (n_pids-1)) {
            last_pid_nvram = 0;
        }
    }

    // Shift left all subsequent entries
    int j;
    for (j = i; j < (n_pids - 1); j++) {
        task_items[j] = task_items[j+1];
    }

    n_pids--;

    return 0;
}

static int refresh_pids(void) {
    int i;

    for (i=0; i < n_pids; i++) {
        if((task_items[i] == NULL) || (find_get_pid(task_items[i]->pid) == NULL)) {
            update_pid_list(i);
            i--;
        }

    }

    printk(KERN_INFO "LIST AFTER REFRESH:");
    for(i=0; i<n_pids; i++) {
        printk(KERN_INFO "i:%d, pid:%d\n", i, task_items[i]->pid);
    }

    return 0;
}



/*
-------------------------------------------------------------------------------

CALLBACK FUNCTIONS

-------------------------------------------------------------------------------
*/


static int pte_callback_mem(pte_t *ptep, unsigned long addr, unsigned long next,
                        struct mm_walk *walk) {

    // If found all save last addr
    if (n_found == n_to_find) {
        last_addr_dram = addr;
        return 1;
    }

    // If page is not present, write protected, or not in DRAM node
    if ((ptep == NULL) || !pte_present(*ptep) || !pte_write(*ptep) || !contains(pfn_to_nid(pte_pfn(*ptep)), DRAM_MODE)) {
        return 0;
    }

    if (!pte_young(*ptep)) {

        // Send to NVRAM
        found_addrs[n_found].addr = addr;
        found_addrs[n_found++].pid_retval = curr_pid;
        return 0;
    }

    if (!pte_dirty(*ptep) && (n_backup < (n_to_find - n_found))) {
            // Add to backup list
            backup_addrs[n_backup].addr = addr;
            backup_addrs[n_backup++].pid_retval = curr_pid;
    }

    pte_t old_pte = ptep_modify_prot_start(walk->vma, addr, ptep);
    *ptep = pte_mkold(old_pte); // unset modified bit
    *ptep = pte_mkclean(old_pte); // unset dirty bit
    ptep_modify_prot_commit(walk->vma, addr, ptep, old_pte, *ptep);

    return 0;
}

/*static int pte_callback_mem_bal(pte_t *ptep, unsigned long addr, unsigned long next,
                        struct mm_walk *walk) {

    // If already found n pages, page is not present or page is not in DRAM node
    if ((ptep == NULL) || (n_found >= n_to_find) || !pte_present(*ptep) || !pte_write(*ptep) || !contains(pfn_to_nid(pte_pfn(*ptep)), DRAM_MODE)) {
        if ((n_found == n_to_find) && !found_last) { // found all + last
            last_addr_dram = addr;
            found_last = 1;
        }
        return 0;
    }

    if (!pte_dirty(*ptep) && pte_young(*ptep)) {
        found_addrs[n_found].addr = addr;
        found_addrs[n_found++].pid_retval = curr_pid;
        return 0;
    }

    pte_t old_pte = ptep_modify_prot_start(walk->vma, addr, ptep);
    *ptep = pte_mkold(old_pte); // unset modified bit
    *ptep = pte_mkclean(old_pte); // unset dirty bit
    ptep_modify_prot_commit(walk->vma, addr, ptep, old_pte, *ptep);

    return 0;
}*/

/* With write isolation never call this
static int pte_callback_mem_force(pte_t *ptep, unsigned long addr, unsigned long next,
                        struct mm_walk *walk) {

    // If already found n pages, page is not present or page is not in DRAM node
    if ((ptep == NULL) || (n_found >= n_to_find) || !pte_present(*ptep) || !pte_write(*ptep) || !contains(pfn_to_nid(pte_pfn(*ptep)), DRAM_MODE)) {
        if ((n_found == n_to_find) && !found_last) { // found all + last
            last_addr_dram = addr;
            found_last = 1;
        }
        return 0;
    }

    if (!pte_dirty(*ptep)) {

        // Send to NVRAM
        found_addrs[n_found].addr = addr;
        found_addrs[n_found++].pid_retval = curr_pid;
    }
    else {
        // Add to backup list
        if (n_backup < n_to_find) {
            backup_addrs[n_backup].addr = addr;
            backup_addrs[n_backup++].pid_retval = curr_pid;
        }
    }

    return 0;
}
*/

static int pte_callback_nvram_force(pte_t *ptep, unsigned long addr, unsigned long next,
                        struct mm_walk *walk) {

    // If found all save last addr
    if (n_found == n_to_find) {
        last_addr_nvram = addr;
        return 1;
    }

    // If page is not present, write protected, or not in NVRAM node
    if ((ptep == NULL) || !pte_present(*ptep) || !pte_write(*ptep) || !contains(pfn_to_nid(pte_pfn(*ptep)), NVRAM_MODE)) {
        return 0;
    }

    if(pte_young(*ptep) && pte_dirty(*ptep)) {
        // Send to DRAM (priority)
        found_addrs[n_found].addr = addr;
        found_addrs[n_found++].pid_retval = curr_pid;
        return 0;
    }

    if (n_backup < (n_to_find - n_found)) {
        // Add to backup list
        backup_addrs[n_backup].addr = addr;
        backup_addrs[n_backup++].pid_retval = curr_pid;
    }

    pte_t old_pte = ptep_modify_prot_start(walk->vma, addr, ptep);
    *ptep = pte_mkold(old_pte); // unset modified bit
    *ptep = pte_mkclean(old_pte); // unset dirty bit
    ptep_modify_prot_commit(walk->vma, addr, ptep, old_pte, *ptep);

    return 0;
}

// used only for debug in ctl (NVRAM_WRITE_MODE)
static int pte_callback_nvram_write(pte_t *ptep, unsigned long addr, unsigned long next,
                        struct mm_walk *walk) {

    // If found all save last addr
    if (n_found == n_to_find) {
        last_addr_nvram = addr;
        return 1;
    }

    // If page is not present, write protected, or not in NVRAM node
    if ((ptep == NULL) || !pte_present(*ptep) || !pte_write(*ptep) || !contains(pfn_to_nid(pte_pfn(*ptep)), NVRAM_MODE)) {
        return 0;
    }

    if (pte_dirty(*ptep)) {
        if (pte_young(*ptep)) {
            // Send to DRAM (priority)
            found_addrs[n_found].addr = addr;
            found_addrs[n_found++].pid_retval = curr_pid;
        }
        else if (n_backup < (n_to_find - n_found)) {
            // Add to backup list
            backup_addrs[n_backup].addr = addr;
            backup_addrs[n_backup++].pid_retval = curr_pid;
        }
    }

    return 0;
}

static int pte_callback_nvram_intensive(pte_t *ptep, unsigned long addr, unsigned long next,
                        struct mm_walk *walk) {

    // If found all save last addr
    if (n_found == n_to_find) {
        last_addr_nvram = addr;
        return 1;
    }

    // If page is not present, write protected, or not in NVRAM node
    if ((ptep == NULL) || !pte_present(*ptep) || !pte_write(*ptep) || !contains(pfn_to_nid(pte_pfn(*ptep)), NVRAM_MODE)) {
        return 0;
    }

    if(pte_young(*ptep)) {
        if (pte_dirty(*ptep)) {
            // Send to DRAM (priority)
            found_addrs[n_found].addr = addr;
            found_addrs[n_found++].pid_retval = curr_pid;
            return 0;
        }

        if (n_backup < (n_to_find - n_found)) {
            // Add to backup list
            backup_addrs[n_backup].addr = addr;
            backup_addrs[n_backup++].pid_retval = curr_pid;
        }
    }

    return 0;
}
static int pte_callback_nvram_switch(pte_t *ptep, unsigned long addr, unsigned long next,
                        struct mm_walk *walk) {

    // If found all save last addr
    if (n_found == n_to_find) {
        last_addr_nvram = addr;
        return 1;
    }

    // If page is not present, write protected, or not in NVRAM node
    if ((ptep == NULL) || !pte_present(*ptep) || !pte_write(*ptep) || !contains(pfn_to_nid(pte_pfn(*ptep)), NVRAM_MODE)) {
        return 0;
    }

    if(pte_young(*ptep)) {
        if (pte_dirty(*ptep)) {
            // Send to DRAM (priority)
            found_addrs[n_found].addr = addr;
            found_addrs[n_found++].pid_retval = curr_pid;
        }

        // Add to backup list
        else if (n_switch_backup < (n_to_find - n_found)) {
            switch_backup_addrs[n_switch_backup].addr = addr;
            switch_backup_addrs[n_switch_backup++].pid_retval = curr_pid;
        }
    }

    return 0;
}

static int pte_callback_nvram_clear(pte_t *ptep, unsigned long addr, unsigned long next,
                        struct mm_walk *walk) {

    // If  page is not present, write protected, or page is not in NVRAM node
    if ((ptep == NULL) || !pte_present(*ptep) || !pte_write(*ptep) || !contains(pfn_to_nid(pte_pfn(*ptep)), NVRAM_MODE)) {
        return 0;
    }

    pte_t old_pte = ptep_modify_prot_start(walk->vma, addr, ptep);
    *ptep = pte_mkold(old_pte); // unset modified bit
    *ptep = pte_mkclean(old_pte); // unset dirty bit
    ptep_modify_prot_commit(walk->vma, addr, ptep, old_pte, *ptep);

    return 0;
}

/*static int pte_callback_count_dram(pte_t *ptep, unsigned long addr, unsigned long next,
                        struct mm_walk *walk) {

    if ((ptep == NULL) || !pte_present(*ptep)) {
        return 0;
    }
    if (pte_young(*ptep) && contains(pfn_to_nid(pte_pfn(*ptep)), DRAM_MODE)) {
        found_addrs[0].addr++;
    }

    return 0;
}*/

/*static int pte_callback_count_nvram(pte_t *ptep, unsigned long addr, unsigned long next,
                        struct mm_walk *walk) {

    if ((ptep == NULL) || !pte_present(*ptep)) {
        return 0;
    }
    if (pte_young(*ptep) && contains(pfn_to_nid(pte_pfn(*ptep)), NVRAM_MODE)) {
        found_addrs[0].addr++;
    }

    return 0;
}*/



/*
-------------------------------------------------------------------------------

PAGE WALKERS

-------------------------------------------------------------------------------
*/



static int do_page_walk(struct mm_walk_ops mem_walk_ops, int last_pid, unsigned long last_addr) {
    struct mm_struct *mm;
    int i;
    // begin at last_pid->last_addr
    mm = task_items[last_pid]->mm;
    curr_pid = task_items[last_pid]->pid;

    if(mm != NULL) {
        mmap_read_lock(mm);
        walk_page_range(mm, last_addr, MAX_ADDRESS, &mem_walk_ops, NULL);
        mmap_read_unlock(mm);
    }

    if (n_found >= n_to_find) {
        return last_pid;
    }

    for (i=last_pid+1; i<n_pids; i++) {

        mm = task_items[i]->mm;
        curr_pid = task_items[i]->pid;

        if(mm != NULL) {
            mmap_read_lock(mm);
            walk_page_range(mm, 0, MAX_ADDRESS, &mem_walk_ops, NULL);
            mmap_read_unlock(mm);
        }

        if (n_found >= n_to_find) {
            return i;
        }
    }

    for (i = 0; i < last_pid; i++) {
        mm = task_items[i]->mm;
        curr_pid = task_items[i]->pid;

        if(mm != NULL) {
            mmap_read_lock(mm);
            walk_page_range(mm, 0, MAX_ADDRESS, &mem_walk_ops, NULL);
            mmap_read_unlock(mm);
        }
        if (n_found >= n_to_find) {
            return i;
        }
    }

    // finish cycle at last_pid->last_addr
    mm = task_items[last_pid]->mm;
    curr_pid = task_items[last_pid]->pid;

    if(mm != NULL) {
        mmap_read_lock(mm);
        walk_page_range(mm, 0, last_addr+1, &mem_walk_ops, NULL);
        mmap_read_unlock(mm);
    }

    return last_pid;
}

static int mem_walk(int n, int mode) {
    struct mm_walk_ops mem_walk_ops = {};
    int dram_walk = 0;

    switch (mode) {
        case DRAM_MODE:
            mem_walk_ops.pte_entry = pte_callback_mem;
            dram_walk = 1;
            break;
        case NVRAM_MODE:
            mem_walk_ops.pte_entry = pte_callback_nvram_force;
            break;
        case NVRAM_INTENSIVE_MODE:
            mem_walk_ops.pte_entry = pte_callback_nvram_intensive;
            break;
        case NVRAM_WRITE_MODE:
            mem_walk_ops.pte_entry = pte_callback_nvram_write;
            break;
        default:
            printk("PLACEMENT: Unrecognized mode.\n");
            return 0;
    }

    n_to_find = n;
    n_backup = 0;

    if (dram_walk) {
        last_pid_dram = do_page_walk(mem_walk_ops, last_pid_dram, last_addr_dram);
    }
    else {
        last_pid_nvram = do_page_walk(mem_walk_ops, last_pid_nvram, last_addr_nvram);
    }

    if (n_found >= n_to_find) {
        return 0;
    }
    else if (n_backup > 0) {
        int remaining = n_to_find - n_found;
        int i;

        for (i=0; (i < remaining) && (i < n_backup); i++) {
            found_addrs[n_found].addr = backup_addrs[i].addr;
            found_addrs[n_found++].pid_retval = backup_addrs[i].pid_retval;
        }

        if (n_found >= n_to_find) {
            return 0;
        }
    }
    return -1;
}

static int clear_walk(int mode) {
    struct mm_struct *mm;
    struct mm_walk_ops mem_walk_ops = {.pte_entry = pte_callback_nvram_clear};

    int i;
    for (i=0; i < n_pids; i++) {
        mm = task_items[i]->mm;
        spin_lock(&mm->page_table_lock);
        curr_pid = task_items[i]->pid;
        walk_page_range(mm, 0, MAX_ADDRESS, &mem_walk_ops, NULL);
        spin_unlock(&mm->page_table_lock);
    }

    return 0;
}

/*static int balance_walk(int n, int mode) {
    struct mm_struct *mm;
    struct mm_walk_ops mem_walk_ops = {};

    found_addrs[0].addr = 0;

    if (mode == BALANCE_DRAM_MODE) {
        mem_walk_ops.pte_entry = pte_callback_count_dram;
    }
    else if (mode == BALANCE_NVRAM_MODE) {
        mem_walk_ops.pte_entry = pte_callback_count_nvram;
    }
    else {
        printk("PLACEMENT: Unrecognized balance mode.\n");
        return 0;
    }

    int i;
    for (i=0; i < n_pids; i++) {

        mm = task_items[i]->mm;
        spin_lock(&mm->page_table_lock);
        curr_pid = task_items[i]->pid;
        walk_page_range(mm, 0, MAX_ADDRESS, &mem_walk_ops, NULL);
        spin_unlock(&mm->page_table_lock);
    }

    int pages_found = found_addrs[0].addr;

    if (mode == BALANCE_DRAM_MODE) {
        printk("DRAM");
    }
    else {
        printk("NVRAM");
    }
    printk(KERN_CONT " BALANCE FOUND: %d pages\n", pages_found);

    return pages_found * n / 1000;
} */

static int switch_walk(int n) {
    struct mm_walk_ops mem_walk_ops = {.pte_entry = pte_callback_nvram_switch};

    n_to_find = n;
    n_switch_backup = 0;

    last_pid_nvram = do_page_walk(mem_walk_ops, last_pid_nvram, last_addr_nvram);

    found_addrs[n_found].pid_retval = 0; // fill separator after
    if ((n_found == 0) && (n_switch_backup == 0)) {
        n_found++;
        return -1;
    }

    int nvram_found = n_found; // store the number of ideal nvram pages found
    int dram_to_find = int_min(nvram_found + n_switch_backup, n);
    n_found++;
    n_to_find = n_found + dram_to_find; // try to find the same amount of dram addrs
    n_backup = 0;

    mem_walk_ops.pte_entry = pte_callback_mem;
    last_pid_dram = do_page_walk(mem_walk_ops, last_pid_dram, last_addr_dram);
    int dram_found = n_found - nvram_found - 1;
    // found equal number of dram and nvram entries
    if (dram_found == nvram_found) {
        return 0;
    }
    else if ((dram_found < nvram_found) && (n_backup > 0)) {
        int remaining = nvram_found - dram_found;
        int to_add;

        if (n_backup < remaining) {
            // shift left dram entries (discard excess nvram addrs)
            int old_dram_start = nvram_found + 1;
            nvram_found = dram_found + n_backup; // update nvram_found and discard other entries
            int new_dram_start = nvram_found + 1;
            found_addrs[nvram_found].pid_retval = 0; // fill separator after nvram pages

            int i;
            for (i = 0; i < dram_found; i++) {
                found_addrs[new_dram_start + i].addr = found_addrs[old_dram_start + i].addr;
                found_addrs[new_dram_start + i].pid_retval = found_addrs[old_dram_start + i].pid_retval;
            }
            to_add = n_backup;
            n_found = new_dram_start + dram_found;
        }
        else {
            to_add = remaining;
        }
        int i;
        for (i = 0; i < to_add; i++) {
            found_addrs[n_found].addr = backup_addrs[i].addr;
            found_addrs[n_found++].pid_retval = backup_addrs[i].pid_retval;
        }

    }
    else if ((nvram_found < dram_found) && (n_switch_backup > 0)) {
        int remaining = dram_found - nvram_found;
        int to_add = int_min(n_switch_backup, remaining);
        int i;
        int old_dram_start = nvram_found + 1;
        int new_dram_start = old_dram_start + to_add;
        dram_found = nvram_found + to_add;

        // shift right dram entries
        for (i = dram_found - 1; i >= 0; i--) {
            found_addrs[new_dram_start + i].addr = found_addrs[old_dram_start + i].addr;
            found_addrs[new_dram_start + i].pid_retval = found_addrs[old_dram_start + i].pid_retval;
        }

        for (i = 0; i < to_add; i++) {
            found_addrs[nvram_found].addr = switch_backup_addrs[i].addr;
            found_addrs[nvram_found++].pid_retval = switch_backup_addrs[i].pid_retval;
        }
        found_addrs[nvram_found].pid_retval = 0;
        n_found = nvram_found * 2 + 1; // discard last entries
    }
    else {
        found_addrs[0].pid_retval = 0;
        n_found = 1;
    }


    return 0;
}



/*
-------------------------------------------------------------------------------

BIND/UNBIND FUNCTIONS

-------------------------------------------------------------------------------
*/



static int bind_pid(pid_t pid) {
    if ((pid <= 0) || (pid > MAX_PID_N)) {
        pr_info("PLACEMENT: Invalid pid value in bind command.\n");
        return -1;
    }
    if (!find_target_process(pid)) {
        pr_info("PLACEMENT: Could not bind pid=%d.\n", pid);
        return -1;
    }

    pr_info("PLACEMENT: Bound pid=%d.\n", pid);
    return 0;
}

static int unbind_pid(pid_t pid) {
    if ((pid <= 0) || (pid > MAX_PID_N)) {
        pr_info("PLACEMENT: Invalid pid value in unbind command.\n");
        return -1;
    }

    // Find which task to remove
    int i;
    for (i = 0; i < n_pids; i++) {
        if ((task_items[i] != NULL) && (task_items[i]->pid == pid)) {
            break;
        }
    }

    if (i == n_pids) {
        pr_info("PLACEMENT: Could not unbind pid=%d.\n", pid);
        return -1;
    }

    update_pid_list(i);
    pr_info("PLACEMENT: Unbound pid=%d.\n", pid);
    return 0;
}



/*
-------------------------------------------------------------------------------

MESSAGE/REQUEST PROCESSING

-------------------------------------------------------------------------------
*/



/* Valid request commands:

BIND [pid]
UNBIND [pid]
FIND [tier] [n]

*/
static void process_req(req_t *req) {
    int ret = -1;
    n_found = 0;
    if (req != NULL) {
        switch (req->op_code) {
            case FIND_OP:
                refresh_pids();
                if (n_pids > 0) {
                    int n = 0;
                    switch (req->mode) {
                        case DRAM_MODE:
                        case NVRAM_MODE:
                        case NVRAM_WRITE_MODE:
                        case NVRAM_INTENSIVE_MODE:
                            n = int_min(MAX_N_FIND, req->pid_n);
                            ret = mem_walk(n, req->mode);
                            break;
                        case NVRAM_CLEAR:
                            clear_walk(req->mode);
                            break;
                        case SWITCH_MODE:
                            n = int_min(MAX_N_SWITCH, req->pid_n);
                            ret = switch_walk(n);
                            break;
                        default:
                            pr_info("PLACEMENT: Unrecognized mode.\n");
                    }
                }
                break;
            case BIND_OP:
                refresh_pids();
                ret = bind_pid(req->pid_n);
                break;
            case UNBIND_OP:
                ret = unbind_pid(req->pid_n);
                refresh_pids();
                break;

            default:
                pr_info("PLACEMENT: Unrecognized opcode.\n");
        }
    }

    found_addrs[n_found++].pid_retval = ret;
}


static void placement_nl_process_msg(struct sk_buff *skb) {
    struct nlmsghdr *nlmh;
    int sender_pid;
    struct sk_buff *skb_out;
    req_t *in_req;
    int res;

    printk("PLACEMENT: Received message.\n");

    // input
    nlmh = (struct nlmsghdr *) skb->data;

    in_req = (req_t *) NLMSG_DATA(nlmh);
    sender_pid = nlmh->nlmsg_pid;

    process_req(in_req);


    // Calculate size of the last netlink packet
    int last_packet_remainder = n_found % MAX_N_PER_PACKET;
    int last_packet_entries = last_packet_remainder;
    if (last_packet_remainder == 0) {
        last_packet_entries = MAX_N_PER_PACKET;
    }

    int required_packets = (n_found / MAX_N_PER_PACKET) + (last_packet_remainder != 0);
    skb_out = nlmsg_new(NLMSG_LENGTH(MAX_PAYLOAD) * required_packets, GFP_KERNEL);
    if (!skb_out) {
        pr_err("Failed to allocate new skb.\n");
        return;
    }

    int i;

    for (i=0; i < required_packets-1; i++) { // process all but last packet
        nlmh_array[i] = nlmsg_put(skb_out, 0, 0, 0, MAX_N_PER_PACKET * sizeof(addr_info_t), NLM_F_MULTI);
        memset(NLMSG_DATA(nlmh_array[i]), 0, MAX_PAYLOAD);
        memcpy(NLMSG_DATA(nlmh_array[i]), found_addrs + i*MAX_N_PER_PACKET, MAX_PAYLOAD);
    }
    int rem_size = last_packet_entries * sizeof(addr_info_t);

    nlmh_array[i] = nlmsg_put(skb_out, 0, 0, NLMSG_DONE, rem_size, 0);
    memset(NLMSG_DATA(nlmh_array[i]), 0, rem_size);
    memcpy(NLMSG_DATA(nlmh_array[i]), found_addrs + i*MAX_N_PER_PACKET, rem_size);

    NETLINK_CB(skb_out).dst_group = 0; // unicast

    if (n_found == 1) {
        pr_info("PLACEMENT: Sending %d entry to ctl.\n", n_found);
    }
    else {
        pr_info("PLACEMENT: Sending %d entries to ctl in %d packets.\n", n_found, required_packets);
    }
    if ((res = nlmsg_unicast(nl_sock, skb_out, sender_pid)) < 0) {
            pr_info("PLACEMENT: Error sending response to ctl.\n");
    }
}



/*
-------------------------------------------------------------------------------

MODULE INIT/EXIT

-------------------------------------------------------------------------------
*/



static int __init _on_module_init(void) {
    pr_info("PLACEMENT-HYB: Hello from module!\n");

    task_items = kmalloc(sizeof(struct task_struct *) * MAX_PIDS, GFP_KERNEL);
    found_addrs = kmalloc(sizeof(addr_info_t) * MAX_N_FIND, GFP_KERNEL);
    backup_addrs = kmalloc(sizeof(addr_info_t) * MAX_N_FIND, GFP_KERNEL);
    switch_backup_addrs = kmalloc(sizeof(addr_info_t) * MAX_N_SWITCH, GFP_KERNEL);
    nlmh_array = kmalloc(sizeof(struct nlmsghdr *) * MAX_PACKETS, GFP_KERNEL);

    struct netlink_kernel_cfg cfg = {
        .input = placement_nl_process_msg,
    };

    nl_sock = netlink_kernel_create(&init_net, NETLINK_USER, &cfg);
    if (!nl_sock) {
        pr_alert("PLACEMENT: Error creating netlink socket.\n");
        return 1;
    }

    return 0;
}

static void __exit _on_module_exit(void) {
    pr_info("PLACEMENT-HYB: Goodbye from module!\n");
    netlink_kernel_release(nl_sock);

    kfree(task_items);
    kfree(found_addrs);
    kfree(backup_addrs);
    kfree(switch_backup_addrs);
    kfree(nlmh_array);
}

module_init(_on_module_init);
module_exit(_on_module_exit);
