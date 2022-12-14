#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "fcntl.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void
trapinit(void)
{
    initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
    w_stvec((uint64)kernelvec);
}

// mp2
// find corresponding vma and vm_block
static int findVMA(uint64 va, struct vma **VMA, struct proc *p){
    struct vm_block *ptr;
    for(int i = 0; i < 16; i++){
        ptr = p->vmas[i].vm_head; 
        if(ptr == 0)
            continue;
        while(ptr->next != 0){
            ptr = ptr->next;
            if(ptr->addr == va){
                *VMA = p->vmas+i;
                return 1;
            }
        }
    }
    return 0;
}
int mmap_allocate(uint64 va, int scause, struct proc *p){
    struct vma *VMA = 0;
    va = PGROUNDDOWN(va);
    if(findVMA(va, &VMA, p) == 0 ){
        //printf("not mmap page fault\n");
        goto bad;
    }
    int pte_per = 0;
    if(VMA->vm_prot & PROT_READ)
        pte_per |= PTE_R;
    if(VMA->vm_prot & PROT_WRITE)
        pte_per |= PTE_W;
    if(scause == 13 && !(pte_per & PTE_R) ){
        //printf("lack read permission\n");
        goto bad;
    }
    if(scause == 15 && !(pte_per & PTE_W) ){
        //printf("lack write permission\n");
        goto bad;
    }
    pte_per |= PTE_V;
    pte_per |= PTE_U;

    // child will copy its memory from parent
    // if MAP_SHARED is set on
    if(p->parent->pid != 2 && (VMA->vm_flags & MAP_SHARED) &&
       walkaddr(p->parent->pagetable, va) != 0){
        char *tmp = kalloc();
        uvmalloc_prot(p->pagetable, va, va+PGSIZE, pte_per);
        if( copyin(p->parent->pagetable, tmp, va, PGSIZE) == -1){
        //    printf("copyin fail\n");
            goto bad;
        }
        if( copyout(p->pagetable, va, tmp, PGSIZE) == -1){
        //    printf("copyout fail\n"); 
            goto bad;
        }
        kfree(tmp);
        return 0;
    }

    // grow memory and map, same as sbrk
    // uvmalloc_prot is written by me at kernel/vm.c
    if( (uvmalloc_prot(p->pagetable, va, va+PGSIZE, pte_per)) == 0){
        //printf("grow proc fail\n");
        goto bad;
    }

    // read the content of the file
    // notice fileread will change the offset of the file
    // we must recover it
    uint64 old_offset = VMA->vm_file->off;
    VMA->vm_file->off = va-(VMA->vm_head->next->addr);
    if(fileread(VMA->vm_file, va, PGSIZE) < 0){
        //printf("fileread error\n");
        goto bad;
    }
    VMA->vm_file->off = old_offset;

    return 0;
 bad:
    return -1;
}
//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void)
{
    int which_dev = 0;

    if((r_sstatus() & SSTATUS_SPP) != 0)
        panic("usertrap: not from user mode");

    // send interrupts and exceptions to kerneltrap(),
    // since we're now in the kernel.
    w_stvec((uint64)kernelvec);

    struct proc *p = myproc();
    
    // save user program counter.
    p->trapframe->epc = r_sepc();
    
    if(r_scause() == 8){
        // system call

        if(p->killed)
            exit(-1);

        // sepc points to the ecall instruction,
        // but we want to return to the next instruction.
        p->trapframe->epc += 4;

        // an interrupt will change sstatus &c registers,
        // so don't enable until done with those registers.
        intr_on();

        syscall();
    } else if((which_dev = devintr()) != 0){
        // ok
    } else if(r_scause() == 13 || r_scause() == 15){
        if(mmap_allocate(r_stval(), r_scause(), myproc()) == 0){
        }
        else{
            printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
            printf("                        sepc=%p stval=%p\n", r_sepc(), r_stval());
            p->killed = 1;
        }
    }else {
        printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
        printf("                        sepc=%p stval=%p\n", r_sepc(), r_stval());
        p->killed = 1;
    }

    if(p->killed)
        exit(-1);

    // give up the CPU if this is a timer interrupt.
    if(which_dev == 2)
        yield();

    usertrapret();
}

//
// return to user space
//
void
usertrapret(void)
{
    struct proc *p = myproc();

    // we're about to switch the destination of traps from
    // kerneltrap() to usertrap(), so turn off interrupts until
    // we're back in user space, where usertrap() is correct.
    intr_off();

    // send syscalls, interrupts, and exceptions to trampoline.S
    w_stvec(TRAMPOLINE + (uservec - trampoline));

    // set up trapframe values that uservec will need when
    // the process next re-enters the kernel.
    p->trapframe->kernel_satp = r_satp();                 // kernel page table
    p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
    p->trapframe->kernel_trap = (uint64)usertrap;
    p->trapframe->kernel_hartid = r_tp();                 // hartid for cpuid()

    // set up the registers that trampoline.S's sret will use
    // to get to user space.
    
    // set S Previous Privilege mode to User.
    unsigned long x = r_sstatus();
    x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
    x |= SSTATUS_SPIE; // enable interrupts in user mode
    w_sstatus(x);

    // set S Exception Program Counter to the saved user pc.
    w_sepc(p->trapframe->epc);

    // tell trampoline.S the user page table to switch to.
    uint64 satp = MAKE_SATP(p->pagetable);

    // jump to trampoline.S at the top of memory, which 
    // switches to the user page table, restores user registers,
    // and switches to user mode with sret.
    uint64 fn = TRAMPOLINE + (userret - trampoline);
    ((void (*)(uint64,uint64))fn)(TRAPFRAME, satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
    int which_dev = 0;
    uint64 sepc = r_sepc();
    uint64 sstatus = r_sstatus();
    uint64 scause = r_scause();
    
    if((sstatus & SSTATUS_SPP) == 0)
        panic("kerneltrap: not from supervisor mode");
    if(intr_get() != 0)
        panic("kerneltrap: interrupts enabled");

    if((which_dev = devintr()) == 0){
        printf("scause %p\n", scause);
        printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
        panic("kerneltrap");
    }

    // give up the CPU if this is a timer interrupt.
    if(which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
        yield();

    // the yield() may have caused some traps to occur,
    // so restore trap registers for use by kernelvec.S's sepc instruction.
    w_sepc(sepc);
    w_sstatus(sstatus);
}

void
clockintr()
{
    acquire(&tickslock);
    ticks++;
    wakeup(&ticks);
    release(&tickslock);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
    uint64 scause = r_scause();

    if((scause & 0x8000000000000000L) &&
         (scause & 0xff) == 9){
        // this is a supervisor external interrupt, via PLIC.

        // irq indicates which device interrupted.
        int irq = plic_claim();

        if(irq == UART0_IRQ){
            uartintr();
        } else if(irq == VIRTIO0_IRQ){
            virtio_disk_intr();
        } else if(irq){
            printf("unexpected interrupt irq=%d\n", irq);
        }

        // the PLIC allows each device to raise at most one
        // interrupt at a time; tell the PLIC the device is
        // now allowed to interrupt again.
        if(irq)
            plic_complete(irq);

        return 1;
    } else if(scause == 0x8000000000000001L){
        // software interrupt from a machine-mode timer interrupt,
        // forwarded by timervec in kernelvec.S.

        if(cpuid() == 0){
            clockintr();
        }
        
        // acknowledge the software interrupt by clearing
        // the SSIP bit in sip.
        w_sip(r_sip() & ~2);

        return 2;
    } else {
        return 0;
    }
}

