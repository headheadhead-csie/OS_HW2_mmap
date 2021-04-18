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
#include "virtio.h"
#include "fs.h"
#include "file.h"

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

//mp2
uint64 sys_mmap(void){
    struct proc *p = myproc();
    void *addr;
    size_t length;
    int prot;
    int flags;
    int fd;
    off_t offset;
    argaddr(0, (uint64 *)&addr), argaddr(1, &length), argint(2, &prot),
    argint(3, &flags), argint(4, &fd), argaddr(5, &offset);

    struct file *f = p->ofile[fd];
    if( addr != 0 ){
        panic("addr should be 0");
        goto bad;
    }
    if( length < 0 ){
        panic("length should be greater than 0");
        goto bad;
    }
    if( prot < 0 || prot > 7){
        panic("invalid prot");
        goto bad;
    }
    if( !(flags & (MAP_SHARED | MAP_PRIVATE) ) ){
        panic("invalid flags");
        goto bad;
    }
    if( f == 0){
        panic("invalid fd");
        goto bad;
    }
    if( f->type != FD_INODE ){
        panic("invalid file");
        goto bad;
    }
    if( offset < 0){
        panic("invalid offset");
        goto bad;
    }

    struct vma VMA;
    VMA.vm_addr = PGROUNDUP(p->sz);
    VMA.vm_file = f;
    VMA.vm_length = length;
    VMA.vm_flags = flags;
    VMA.vm_pgoff = offset;//offset within file
    
    length = PGROUNDUP(length);

    // grow memory and map, same as sbrk
    if( growproc(length) < 0)
        panic("growproc fail");

    //read the content of file
    fileread(f, VMA.vm_addr, length);

    // increase file's reference count
    VMA.vm_file->ref++;
     
    int i;
    for(i = 0; i < 16; i++){
        if(p->vmas[i].vm_addr == 0){
            p->vmas[i] = VMA;
            break;
        }
    }
    return p->vmas[i].vm_addr;
 bad:
    return -1;
}
uint64 sys_munmap(void){
    return 0;
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
