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
    int pte_flag = 0;
    off_t offset;
    argaddr(0, (uint64 *)&addr), argaddr(1, &length), argint(2, &prot),
    argint(3, &flags), argint(4, &fd), argaddr(5, &offset);

    struct file *f = p->ofile[fd];
    if( addr != 0 ){
        printf("addr should be 0\n");
        goto bad;
    }
    if( length < 0 ){
        printf("length should be greater than 0\n");
        goto bad;
    }
    if( prot < 0 || prot > 7){
        printf("invalid prot\n");
        goto bad;
    }
    if( !(flags & (MAP_SHARED | MAP_PRIVATE) ) ){
        printf("invalid flags\n");
        goto bad;
    }
    if( f == 0){
        printf("invalid fd\n");
        goto bad;
    }
    if( f->type != FD_INODE ){
        printf("invalid file\n");
        goto bad;
    }
    if( offset < 0){
        printf("invalid offset\n");
        goto bad;
    }

    // handle permission issue
    printf("writable:%d, readable:%d\n", f->writable, f->readable);
    if(prot & PROT_READ){
        pte_flag |= PTE_R;
        if(!(f->readable)){
            printf("file is not readable\n");
            goto bad;
        }
    }
    if(prot & PROT_WRITE){
        pte_flag |= PTE_W;
        if(!(f->writable) && (flags & MAP_SHARED)){
            printf("file is not writable\n");
            goto bad;
        }
    }
    if(prot & PROT_EXEC)
        pte_flag |= PTE_X;

    struct vma VMA;
    VMA.vm_addr = PGROUNDUP(p->sz);
    VMA.vm_file = f;
    VMA.vm_length = length;
    VMA.vm_flags = flags;
    VMA.vm_pgoff = offset;//offset within file
    
    length = PGROUNDUP(length);
    offset = PGROUNDUP(offset);

    // grow memory and map, same as sbrk
    if( (p->sz= uvmalloc_prot(p->pagetable, p->sz, p->sz+length, pte_flag)) == 0){
        printf("grow proc fail\n");
        goto bad;
    }

    // read the content of file
    // notice fileread will change the offset of f
    // we must recover it
    uint old_offset = f->off;
    fileread(f, VMA.vm_addr, length);
    f->off = old_offset;

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
