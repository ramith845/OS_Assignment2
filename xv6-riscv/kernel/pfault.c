/* This file contains code for a generic page fault handler for processes. */
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

int loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz);
int flags2perm(int flags);

/* CSE 536: (2.4) read current time. */
uint64 read_current_timestamp() {
  uint64 curticks = 0;
  acquire(&tickslock);
  curticks = ticks;
  wakeup(&ticks);
  release(&tickslock);
  return curticks;
}

bool psa_tracker[PSASIZE];

/* All blocks are free during initialization. */
void init_psa_regions(void)
{
    for (int i = 0; i < PSASIZE; i++) 
        psa_tracker[i] = false;
}

/* Evict heap page to disk when resident pages exceed limit */
void evict_page_to_disk(struct proc* p) {
    /* Find free block */
    int blockno = -1;
    for (size_t i = 0; i <= PSAEND - 4; i++)
    {
        if (!psa_tracker[i] && !psa_tracker[i+1]
            && !psa_tracker[i+2] && !psa_tracker[i+3])
        {
            blockno = (int)i;
            for (size_t j = 0; j < 4; j++)
            {
                psa_tracker[i + j] = true;
            }
            
            break;
        }
    }
    
    int victim = -1;
    uint64 currTime = read_current_timestamp();

    /* Working Set Algorithm (WSA): prefer pages that have not been accessed
     * recently. We check the PTE_A bit for each loaded heap page. If the A bit
     * is set, update last_load_time and clear A. Otherwise compute age and if
     * age >= WS_TAU_TICKS consider it for eviction. If no candidate found,
     * fall back to oldest (FIFO-style) based on last_load_time.
     */
    uint64 oldest_age = 0;
    int fifo_candidate = -1;
    uint64 fifo_max = 0;

    for (size_t i = 0; i < MAXHEAP; i++) {
        if (!(p->heap_tracker[i].loaded) || p->heap_tracker[i].startblock != -1)
            continue;
        uint64 va = p->heap_tracker[i].addr;
        pte_t *pte = walk(p->pagetable, va, 0);
        if (pte == 0)
            continue;

        /* If accessed, refresh last_load_time and clear accessed bit. */
        if (*pte & PTE_A) {
            p->heap_tracker[i].last_load_time = currTime;
            *pte &= ~PTE_A;
            continue;
        }

        uint64 age = currTime - p->heap_tracker[i].last_load_time;
        if (age >= WS_TAU_TICKS && (uint64)age > oldest_age) {
            oldest_age = age;
            victim = (int)i;
        }

        /* Track FIFO candidate (oldest overall) as fallback. */
        if (p->heap_tracker[i].last_load_time > 0) {
            uint64 age_fifo = currTime - p->heap_tracker[i].last_load_time;
            if (age_fifo > fifo_max) {
                fifo_max = age_fifo;
                fifo_candidate = (int)i;
            }
        }
    }

    if (victim == -1) {
        /* No WSA candidate; use FIFO fallback */
        if (fifo_candidate != -1)
            victim = fifo_candidate;
    }

    if (victim == -1) {
        /* As a last resort, scan and pick first loaded page */
        for (size_t i = 0; i < MAXHEAP; i++) {
            if (p->heap_tracker[i].loaded && p->heap_tracker[i].startblock == -1) {
                victim = (int)i;
                break;
            }
        }
    }
    
    /* Print statement. */
    if (victim == -1) {
        // nothing to evict
        for (size_t j = 0; j < 4; j++) {
            psa_tracker[blockno + j] = false;
        }
        return;
    }

    print_evict_page(p->heap_tracker[victim].addr, blockno);
    /* Read memory from the user to kernel memory first. */
    char *mem = kalloc(); // TODO
    copyin(p->pagetable, mem, p->heap_tracker[victim].addr, PGSIZE); 

    /* Write to the disk blocks. Below is a template as to how this works. There is
     * definitely a better way but this works for now. :p */
    struct buf* b;
    for (size_t i = 0; i < 4; i++)
    {   
        b = bread(1, PSASTART + (blockno) + i);
        // Copy page contents to b.data using memmove.
        memmove(b->data, mem + i * BSIZE , BSIZE);
        bwrite(b);
        brelse(b);
    }
    kfree(mem);
    /* Unmap swapped out page */
    uvmunmap(p->pagetable, p->heap_tracker[victim].addr, 1, true);
    /* Update the resident heap tracker. */
    p->heap_tracker[victim].startblock = blockno;
    p->heap_tracker[victim].loaded = false;
    p->resident_heap_pages--;
}

/* Retrieve faulted page from disk. */
void retrieve_page_from_disk(struct proc* p, uint64 uvaddr) {
    /* Find where the page is located in disk */
    int block_start = 0;
    int head_id = -1;
    uint64 uvaddr_aligned = PGROUNDDOWN(uvaddr);
    for (size_t i = 0; i < MAXHEAP; i++)
    {
        if (p->heap_tracker[i].addr == uvaddr_aligned && 
            !p->heap_tracker[i].loaded &&
            p->heap_tracker[i].startblock != -1) 
        {
            block_start = p->heap_tracker[i].startblock;
            head_id = (int)i;
            break;
        }
    }
    
    /* Print statement. */
    print_retrieve_page(uvaddr, block_start);

    /* Create a kernel page to read memory temporarily into first. */
    char *mem = kalloc();

    /* Read the disk block into temp kernel page. */
    struct buf* b;
    for (size_t i = 0; i < 4; i++)
    {   
        b = bread(1, PSASTART + (block_start) + i);
        // Copy page contents to b.data using memmove.
        memmove(mem + i * BSIZE, b->data , BSIZE);
        brelse(b);
    }
    /* Copy from temp kernel page to uvaddr (use copyout) */
    copyout(p->pagetable, uvaddr_aligned, mem, PGSIZE);

    kfree(mem);

    p->heap_tracker[head_id].loaded = true;
    p->heap_tracker[head_id].startblock = -1;
    p->heap_tracker[head_id].last_load_time = read_current_timestamp();

    for(int i = 0; i < 4; i++) {
        psa_tracker[block_start + i] = false;
    }
}


void page_fault_handler(void) 
{
    // printf("Page Fault Handler START\n");
    
    /* Current process struct */
    struct proc *p = myproc();

    /* Track whether the heap page should be brought back from disk or not. */
    bool load_from_disk = false;

    /* Find faulting address. */
    uint64 faulting_addr = r_stval();
    uint64 faulting_addr_aligned = PGROUNDDOWN(faulting_addr);
    print_page_fault(p->name, faulting_addr_aligned);

    
    if (p->cow_enabled && p->cow_group && r_scause() == 15)
    {
        copy_on_write();
        goto out;
    }

    /* Check if the fault address is a heap page. Use p->heap_tracker */
    int heap_id = -1;
    for (size_t i = 0; i < MAXHEAP; i++)
    {
        if (p->heap_tracker[i].addr == faulting_addr_aligned 
            && faulting_addr < p->heap_tracker[i].addr + PGSIZE) 
        {
            heap_id = (int)i;
            goto heap_handle;
        }
    }
    
    /* If it came here, it is a page from the program binary that we must load. */

    struct elfhdr elf;
    struct proghdr ph;
    struct inode *ip;
    uint64 argc, sz = 0;
    pagetable_t pagetable = 0, oldpagetable;
    int i, off;
    
    begin_op();                       // fs transaction (like exec)
    ip = namei(p->name);              // re-open the binary by filename
    // if(ip == 0){ end_op(); goto bad; }
    ilock(ip);

    // read ELF header
    readi(ip, 0, (uint64)&elf, 0, sizeof(elf));
    // if(elf.magic != ELF_MAGIC) goto bad;
    pagetable = p->pagetable;
    for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
        readi(ip, 0, (uint64)&ph, off, sizeof(ph));
        if(ph.type != ELF_PROG_LOAD)
            continue;
        
        if (faulting_addr_aligned >= ph.vaddr && faulting_addr_aligned < ph.vaddr + ph.memsz) {   
            // printf("Page found FA: %p, PH_VADDR: %p, PH_SIZE: %p\n", faulting_addr_aligned, ph.vaddr, ph.memsz);
            uint64 sz1;
            if((sz1 = uvmalloc(pagetable, faulting_addr_aligned, faulting_addr_aligned + PGSIZE, flags2perm(ph.flags) | PTE_U | PTE_R | PTE_V)) == 0)
                printf("[ERROR] Allocate physical memory\n");
            sz = sz1;
            uint offset_in_file = ph.off + (faulting_addr_aligned - ph.vaddr);
            if(loadseg(pagetable, faulting_addr_aligned, ip, offset_in_file, PGSIZE) < 0)
                printf("[ERROR] Loading to physical memory\n");
            
            print_load_seg(faulting_addr_aligned, ph.off, ph.memsz);
            break;
        }
    }

    iunlockput(ip);
    end_op();

    
    /* Go to out, since the remainder of this code is for the heap. */
    goto out;

heap_handle:

    pte_t *pte = walk(p->pagetable, faulting_addr_aligned, 0);
    if (pte && (*pte & PTE_V) && !(*pte & PTE_W) && r_scause() == 15) {
        // printf("WRITE Fault make it writable");
        *pte |= PTE_W;
        sfence_vma();
        goto out;
    }

    load_from_disk = (!p->heap_tracker[heap_id].loaded && p->heap_tracker[heap_id].startblock != -1);
    /* 2.4: Check if resident pages are more than heap pages. If yes, evict. */
    if (p->resident_heap_pages == MAXRESHEAP) {
        evict_page_to_disk(p);
    }

    /* 2.3: Map a heap page into the process' address space. (Hint: check growproc) */
    if((sz = uvmalloc(p->pagetable, faulting_addr_aligned, faulting_addr_aligned + PGSIZE, PTE_W)) == 0)
        return -1;
    
    /* 2.4: Heap page was swapped to disk previously. We must load it from disk. */
    if (load_from_disk) {
        retrieve_page_from_disk(p, faulting_addr);
    }
    
    /* 2.4: Update the last load time for the loaded heap page in p->heap_tracker. */
    p->heap_tracker[heap_id].loaded = 1;
    p->heap_tracker[heap_id].last_load_time = read_current_timestamp();

    /* Track that another heap page has been brought into memory. */
    p->resident_heap_pages++;

out:
    // printf("--------------------------------------\n");
    // printf("Page Fault Handled\n\n");
    /* Flush stale page table entries. This is important to always do. */
    sfence_vma();
    return;
}