#include <trap.h>
#include <spinlock.h>
#include <uart.h>
#include <debug.h>
#include <scheduler.h>
#include <printf.h>
#include <plic.h>
#include <virtio_disk.h>
#include <debug.h>

extern void kernel_vec(void);
extern char trampoline[], user_vec[], user_ret[];
extern void syscall(void);

void user_trap_ret(void);

struct spinlock tick_lock;
/* For test */
volatile uint64 tick_count = 0;

static void tick_intr(void)
{
        spinlock_acquire(&tick_lock);

        tick_count ++;
        if(tick_count % 10 == 0) {
                // printf("timer interrupt\n");
        }
        wakeup((void*)&tick_count);
        spinlock_release(&tick_lock);
}

static int dev_intr(uint64 scause)
{
        int irq = 0;
        /* This is a supervisor external interrupt via PLIC */
        if((scause & 0x8000000000000000L) &&
           (scause & 0xff) == 9) {
                /* Get interrupt number from PLIC */
                irq = plic_claim();
                if(irq == UART0_IRQ) {
                        uart_intr();
                } else if(irq == VIRTIO0_IRQ) {
                        virtio_disk_intr();
                /* 
                 * irq 0 is reserved to mean “no interrupt”.
                 * see here: https://five-embeddev.com/riscv-priv-isa-manual/Priv-v1.12/plic.html#interrupt-identifiers-ids
                 */
                } else if(irq != 0){
                        printf("Unexpected interrupt irq=%d\n", irq);
                }
                if(irq) {
                        /* Clear the interrupt flag */
                        plic_complete(irq);
                }
                return 1;
        } else if(scause == 0x8000000000000001L) {
                /* Software interrupt */
                if(cpuid() == 0) {
                        tick_intr();
                } 
                 /* Clear the interrupt flag */
                sip_w(sip_r() &~ 2);
                return 2;
        }   
        return 0;
}

void kernel_trap(void)
{
        uint8 which_dev = 0;
        uint64 sepc = sepc_r();
        uint64 sstatus = sstatus_r();
        uint64 cause = scause_r();

        if(!(sstatus & SSTATUS_SPP)) 
                PANIC("[kernel_trap]It is not from supervisor mode");
        if(intr_status())
                PANIC("[kernel_trap]Interrupt enabled");

        /* Unknown device interrupt */
        if((which_dev = dev_intr(cause)) == 0) {
                printf("scause %p\n", cause);
                printf("sepc=%p stval=%p\n", sepc, stval_r());
                PANIC("kerneltrap");
        }

        if(which_dev == 2 && cur_proc() != 0 && cur_proc()->state == RUNNING)
                yield();

        sepc_w(sepc);
        sstatus_w(sstatus);
}

extern void exit(int cause);
void user_trap_entry(void)
{
        int which_dev = 0;
        process_t p = cur_proc();
        uint64 cause = scause_r();

        if((sstatus_r() & SSTATUS_SPP)) {
                PANIC("Not from user mode");
        }

        stvec_w((uint64)kernel_vec);

        p->cur_thread->trapframe->epc = sepc_r();

        if(cause == 8) {
                if(killed(p))
                        exit(-1);

                /* System call */
                p->cur_thread->trapframe->epc += 4;
                intr_on();
                syscall();
        } else {
                if((which_dev = dev_intr(cause)) == 0) {
                        printf("scause %p\n", cause);
                        printf("sepc=%p stval=%p\n", p->cur_thread->trapframe->epc, stval_r());
                        PANIC("user_trap_entry");
                }
        }

        if(killed(p))
                exit(-1);

        if(which_dev == 2)
                yield();

        user_trap_ret();
}

void user_trap_ret(void)
{
        process_t p;
        uint64 sstatus;
        uint64 satp;
        uint64 trampoline_uservec;
        uint64 trampoline_userret;

        p = cur_proc();
        /* Turn off the interrupt until we' are back in user space */
        intr_off();

        trampoline_uservec = TRAMPOLINE + (user_vec - trampoline);
        stvec_w(trampoline_uservec);

        p->cur_thread->trapframe->kernel_satp = satp_r();
        p->cur_thread->trapframe->kernel_sp = p->cur_thread->kstack + PGSIZE;
        p->cur_thread->trapframe->kernel_hartid = tp_r();
        p->cur_thread->trapframe->kernel_trap = (uint64)user_trap_entry;

        sstatus = sstatus_r();
        /* Set the interrupt is from user mode */
        sstatus &= ~SSTATUS_SPP; 
        /* Enable interrupt */
        sstatus |= SSTATUS_SPIE; 
        sstatus_w(sstatus);

        /* Write the epc. It will be set to 0 if the process is first started */
        sepc_w(p->cur_thread->trapframe->epc);

        satp = MAKE_SATP(p->pagetable);

        trampoline_userret = TRAMPOLINE + (user_ret - trampoline);
        /* Call user_ret */
        ((void (*)(uint64))trampoline_userret)(satp);
}

/* This function for first hart */
void trap_init_lock(void)
{
        spinlock_init(&tick_lock, "trap_tick");
}

/* This function for any hart */
void trap_init(void)
{
        stvec_w((uint64)kernel_vec);
}