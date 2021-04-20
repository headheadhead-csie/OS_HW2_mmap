#include "fcntl.h"
#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "stat.h"

uint64
sys_exit(void)
{
    int n;
    if(argint(0, &n) < 0)
        return -1;
    exit(n);
    return 0;    // not reached
}

uint64
sys_getpid(void)
{
    return myproc()->pid;
}

uint64
sys_fork(void)
{
    return fork();
}

uint64
sys_wait(void)
{
    uint64 p;
    if(argaddr(0, &p) < 0)
        return -1;
    return wait(p);
}

uint64
sys_sbrk(void)
{
    int addr;
    int n;

    if(argint(0, &n) < 0)
        return -1;
    addr = myproc()->sz;
    if(growproc(n) < 0)
        return -1;
    //myproc()->sz = myproc()->sz+n;
    return addr;
}

uint64
sys_sleep(void)
{
    int n;
    uint ticks0;

    if(argint(0, &n) < 0)
        return -1;
    acquire(&tickslock);
    ticks0 = ticks;
    while(ticks - ticks0 < n){
        if(myproc()->killed){
            release(&tickslock);
            return -1;
        }
        sleep(&ticks, &tickslock);
    }
    release(&tickslock);
    return 0;
}

uint64
sys_kill(void)
{
    int pid;

    if(argint(0, &pid) < 0)
        return -1;
    return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
    uint xticks;

    acquire(&tickslock);
    xticks = ticks;
    release(&tickslock);
    return xticks;
}

// mp2
//
// Some code block is comment out because it should be 
// implemented at trap.c
/*
static void listprint(struct vm_block *head){
    if(head == 0)
        return;
    struct vm_block *ptr = head;
    struct vm_block *next = head->next;
    while(next != 0){
        ptr = next;
        printf("next:%p\n", next->addr);
        next = ptr->next; 
    }
}*/
uint64 sys_mmap(void){
    struct proc *p = myproc();
    struct vm_addr *addr;
    size_t length;
    int prot;
    int flags;
    int fd;
    off_t offset;
    argaddr(0, (uint64 *)&addr), argaddr(1, &length), argint(2, &prot),
    argint(3, &flags), argint(4, &fd), argaddr(5, &offset);

    struct file *f = p->ofile[fd];
    if( addr != 0 ){
    //    printf("addr should be 0\n");
        goto bad;
    }
    if( length < 0 ){
    //    printf("length should be greater than 0\n");
        goto bad;
    }
    if( prot < 0 || prot > 7){
    //    printf("invalid prot\n");
        goto bad;
    }
    if( !(flags & (MAP_SHARED | MAP_PRIVATE) ) ){
    //    printf("invalid flags\n");
        goto bad;
    }
    if( f == 0){
    //    printf("invalid fd\n");
        goto bad;
    }
    if( f->type != FD_INODE ){
    //    printf("invalid file\n");
        goto bad;
    }
    if( offset != 0){
    //    printf("invalid offset\n");
        goto bad;
    }

    // handle permission issue
    if(prot & PROT_READ){
        if(!(f->readable)){
    //        printf("file is not readable\n");
            goto bad;
        }
    }
    if(prot & PROT_WRITE){
        if(!(f->writable) && (flags & MAP_SHARED)){
    //        printf("file is not writable\n");
            goto bad;
        }
    }
    // check file size and mmap length
    // seems that there is no need to check this issue?
    // struct stat s;
    // filestat(f, (uint64)&s);
    // if(s.size < length){
    //     printf("file size is not big enough\n");
    //     goto bad;
    // }

    struct vma *VMA = 0;
    int i;
    for(i = 0; i < 16; i++){
        if(p->vmas[i].vm_head == 0){
            VMA = p->vmas+i;
            break;
        }
    }
    if(VMA == 0){
    //    printf("no vma left\n");
        goto bad;
    }
    length = PGROUNDUP(length);
    for(int i = 0; i < mmap_MAXPAGE; i++){
        if(VMA->vm_blocks[i].addr == 0){
            VMA->vm_head = VMA->vm_blocks+i;
            VMA->vm_head->addr = 1;
            break;
        }
    }
    VMA->vm_file = f;
    VMA->vm_length = length;
    VMA->vm_flags = flags;
    VMA->vm_prot = prot;
    VMA->vm_pgoff = offset;//offset within file
    struct vm_block *ptr = VMA->vm_head;
    int num_pages = length/PGSIZE;
    int count = 0;
    // assign address for each block,
    // hasn't allocated yet
    if(p->mmap_sz == 0)
        p->mmap_sz = MAXVA - (mmap_MAXPAGE*16+2)*PGSIZE; 
    for(int i = 0; i < mmap_MAXPAGE && count < num_pages; i++){
        if( VMA->vm_blocks[i].addr == 0 &&
            VMA->vm_blocks+i != VMA->vm_head &&
            VMA->vm_blocks[i].addr != 1){

            ptr->next = (VMA->vm_blocks)+i;
            ptr = ptr->next;
            ptr->addr = p->mmap_sz + count*PGSIZE;
            ptr->offset = count*PGSIZE;
            count++;
        }
    }
    ptr->next = 0;

    // grow memory and map, same as sbrk
    // do it at trap.c
    // if( (uvmalloc_prot(p->pagetable, p->sz, p->sz+length, 31)) == 0){
    //     printf("grow proc fail\n");
    //     goto bad;
    // }
    // if(fileread(VMA->vm_file, p->sz, length) < 0){
    //     printf("fileread error\n");
    //     goto bad;
    // }
    
    // lazy allocation
    p->mmap_sz += length;

    // increase file's reference count
    filedup(VMA->vm_file);
     
    return VMA->vm_head->next->addr;
 bad:
    return -1;
}

static int findVMA(uint64 va, struct vma **VMA, struct vm_block **ptr){
    struct proc *p = myproc();
    struct vm_block *next;
    for(int i = 0; i < 16; i++){
        *ptr = p->vmas[i].vm_head; 
        if(*ptr == 0)
            continue;
        while((*ptr)->next != 0){
            next = (*ptr)->next;
            if(next->addr == va){
                *VMA = p->vmas+i;
                return 1;
            }
            *ptr = next;
        }
    }
    return 0;
}

uint64 sys_munmap(void){
    uint64 addr;
    size_t length;
    argaddr(0, &addr);
    argaddr(1, &length);
    addr = PGROUNDDOWN(addr);
    length = PGROUNDUP(length);

    struct proc *p = myproc();   
    struct vma *VMA;
    struct vm_block *ptr;
    if(!findVMA(addr, &VMA, &ptr)){
    //    printf("invalid addr\n");
        goto bad;
    }           
    int num_pages = length/PGSIZE;

    // after calling findVMA, ptr is the block before the
    // vm_block we want to unmap, in order to maintain linked list
    struct vm_block *start = ptr;
    struct vm_block *next = ptr->next;
    uint64 old_offset;
    while(num_pages > 0 && ptr->next != 0){
        next = ptr->next;

        old_offset = VMA->vm_file->off;
        VMA->vm_file->off = (next->offset);
        writepage(VMA, VMA->vm_file, next->addr, PGSIZE);
        VMA->vm_file->off = old_offset;

        // ensure that the address is mapped
        // otherwise it'll raise unmap error
        if(walkaddr(p->pagetable, next->addr) != 0)
            uvmunmap(p->pagetable, next->addr, 1, 1); 
        // maintain linked list
        next->addr = 0;
        ptr->next = 0;
        ptr = next;
        num_pages--;
    }
    if(ptr->next != 0)
        start->next = ptr->next;
    if(VMA->vm_head->next == 0)
        VMA->vm_head->addr = 0, VMA->vm_head = 0, fileclose(VMA->vm_file);
    for(int i = 0; i < 16; i++){
        VMA = p->vmas+i;
        if(VMA->vm_head != 0 && VMA->vm_head->next != 0)
            return 0;
    }
    p->mmap_sz = 0;
    return 0;
 bad:
    return -1;
}
uint64 sys_vmprint(void){
    pagetable_t pagetable = myproc()->pagetable;
    printf("page table %p\n", pagetable);
    for(int i = 0; i < 512; i++){
        pte_t pte_L2 = pagetable[i];

        if( pte_L2 & PTE_V ){
            pagetable_t child_L1 = (pagetable_t)PTE2PA(pte_L2);
            printf(" ..%d: pte %p pa %p\n", i, pte_L2, child_L1);

            for(int j = 0; j < 512; j++){
                pte_t pte_L1 = child_L1[j];

                if( pte_L1 & PTE_V ){
                    pagetable_t child_L0 = (pagetable_t)PTE2PA(pte_L1);
                    printf(" .. ..%d: pte %p pa %p\n", j, pte_L1, child_L0);

                    for(int k = 0; k < 512; k++){
                        pte_t pte_L0 = child_L0[k];

                        if( pte_L0 & PTE_V ){
                            pagetable_t phys_addr = (pagetable_t)PTE2PA(pte_L0);
                            printf(" .. .. ..%d: pte %p pa %p\n", k, pte_L0, phys_addr);
                        }
                    }
                }
            }
        } 
    }
    return 0;
}
