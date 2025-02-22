#if !C64
#include <assert.h>
#include <fcntl.h>
#include <getopt.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include "reu.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device.h"
#include "riscv.h"
#include "riscv_private.h"
#include "persistence.h"

/* Define fetch separately since it is simpler (fixed width, already checked
 * alignment, only main RAM is executable).
 */
static void mem_fetch(vm_t *vm, uint32_t addr, uint32_t *value)
{
    if (unlikely(addr >= RAM_SIZE)) {
        /* TODO: check for other regions */
        vm_set_exception(vm, RV_EXC_FETCH_FAULT, vm->exc_val);
        return;
    }
#if C64
    *value = loadword_reu(addr& 0xfffffffc);
#else
    emu_state_t *data = (emu_state_t *) vm->priv;
    *value = data->ram[addr >> 2];
#endif
}

static void emu_update_uart_interrupts(vm_t *vm)
{
    emu_state_t *data = (emu_state_t *) vm->priv;
    u8250_update_interrupts(&data->uart);
    if (data->uart.pending_ints)
        data->plic.active |= IRQ_UART_BIT;
    else
        data->plic.active &= ~IRQ_UART_BIT;
    plic_update_interrupts(vm, &data->plic);
}

#if SEMU_HAS(VIRTIONET)
static void emu_update_vnet_interrupts(vm_t *vm)
{
    emu_state_t *data = (emu_state_t *) vm->priv;
    if (data->vnet.InterruptStatus)
        data->plic.active |= IRQ_VNET_BIT;
    else
        data->plic.active &= ~IRQ_VNET_BIT;
    plic_update_interrupts(vm, &data->plic);
}
#endif

#if SEMU_HAS(VIRTIOBLK)
static void emu_update_vblk_interrupts(vm_t *vm)
{
    emu_state_t *data = (emu_state_t *) vm->priv;
    if (data->vblk.InterruptStatus)
        data->plic.active |= IRQ_VBLK_BIT;
    else
        data->plic.active &= ~IRQ_VBLK_BIT;
    plic_update_interrupts(vm, &data->plic);
}
#endif

static void mem_load(vm_t *vm, uint32_t addr, uint8_t width, uint32_t *value)
{
    emu_state_t *data = (emu_state_t *) vm->priv;

    /* RAM at 0x00000000 + RAM_SIZE */
    if (addr < RAM_SIZE) {
        ram_read(vm, data->ram, addr, width, value);
        return;
    }

    if ((addr >> 28) == 0xF) { /* MMIO at 0xF_______ */
        /* 256 regions of 1MiB */
        switch ((addr >> 20) & MASK(8)) {
        case 0x0:
        case 0x2: /* PLIC (0 - 0x3F) */
            plic_read(vm, &data->plic, addr & 0x3FFFFFF, width, value);
            plic_update_interrupts(vm, &data->plic);
            return;
        case 0x40: /* UART */
            u8250_read(vm, &data->uart, addr & 0xFFFFF, width, value);
            emu_update_uart_interrupts(vm);
            return;
#if SEMU_HAS(VIRTIONET)
        case 0x41: /* virtio-net */
            virtio_net_read(vm, &data->vnet, addr & 0xFFFFF, width, value);
            emu_update_vnet_interrupts(vm);
            return;
#endif
#if SEMU_HAS(VIRTIOBLK)
        case 0x42: /* virtio-blk */
            virtio_blk_read(vm, &data->vblk, addr & 0xFFFFF, width, value);
            emu_update_vblk_interrupts(vm);
            return;
#endif
        }
    }
    vm_set_exception(vm, RV_EXC_LOAD_FAULT, vm->exc_val);
}

static void mem_store(vm_t *vm, uint32_t addr, uint8_t width, uint32_t value)
{
    emu_state_t *data = (emu_state_t *) vm->priv;

    /* RAM at 0x00000000 + RAM_SIZE */
    if (addr < RAM_SIZE) {
        ram_write(vm, data->ram, addr, width, value);
        return;
    }

    if ((addr >> 28) == 0xF) { /* MMIO at 0xF_______ */
        /* 256 regions of 1MiB */
        switch ((addr >> 20) & MASK(8)) {
        case 0x0:
        case 0x2: /* PLIC (0 - 0x3F) */
            plic_write(vm, &data->plic, addr & 0x3FFFFFF, width, value);
            plic_update_interrupts(vm, &data->plic);
            return;
        case 0x40: /* UART */
            u8250_write(vm, &data->uart, addr & 0xFFFFF, width, value);
            emu_update_uart_interrupts(vm);
            return;
#if SEMU_HAS(VIRTIONET)
        case 0x41: /* virtio-net */
            virtio_net_write(vm, &data->vnet, addr & 0xFFFFF, width, value);
            emu_update_vnet_interrupts(vm);
            return;
#endif
#if SEMU_HAS(VIRTIOBLK)
        case 0x42: /* virtio-blk */
            virtio_blk_write(vm, &data->vblk, addr & 0xFFFFF, width, value);
            emu_update_vblk_interrupts(vm);
            return;
#endif
        }
    }
    vm_set_exception(vm, RV_EXC_STORE_FAULT, vm->exc_val);
}

/* SBI */
#define SBI_IMPL_ID 0x999
#define SBI_IMPL_VERSION 1

typedef struct {
    int32_t error;
    int32_t value;
} sbi_ret_t;

static inline sbi_ret_t handle_sbi_ecall_TIMER(vm_t *vm, int32_t fid)
{
    emu_state_t *data = (emu_state_t *) vm->priv;
    switch (fid) {
    case SBI_TIMER__SET_TIMER:
        data->timer_lo = vm->x_regs[RV_R_A0];
        data->timer_hi = vm->x_regs[RV_R_A1];
        return (sbi_ret_t){SBI_SUCCESS, 0};
    default:
        return (sbi_ret_t){SBI_ERR_NOT_SUPPORTED, 0};
    }
}

static inline sbi_ret_t handle_sbi_ecall_RST(vm_t *vm, int32_t fid)
{
    emu_state_t *data = (emu_state_t *) vm->priv;
    switch (fid) {
    case SBI_RST__SYSTEM_RESET:
#if !C64
        fprintf(stderr, "system reset: type=%u, reason=%u\n",
                vm->x_regs[RV_R_A0], vm->x_regs[RV_R_A1]);
#else
        printf("system reset: type=%lu, reason=%lu\n",
                vm->x_regs[RV_R_A0], vm->x_regs[RV_R_A1]);
#endif
        data->stopped = true;
        return (sbi_ret_t){SBI_SUCCESS, 0};
    default:
        return (sbi_ret_t){SBI_ERR_NOT_SUPPORTED, 0};
    }
}

#define RV_MVENDORID 0x12345678
#define RV_MARCHID ((1UL << 31) | 1)
#define RV_MIMPID 1

static inline sbi_ret_t handle_sbi_ecall_BASE(vm_t *vm, int32_t fid)
{
    switch (fid) {
    case SBI_BASE__GET_SBI_IMPL_ID:
        return (sbi_ret_t){SBI_SUCCESS, SBI_IMPL_ID};
    case SBI_BASE__GET_SBI_IMPL_VERSION:
        return (sbi_ret_t){SBI_SUCCESS, SBI_IMPL_VERSION};
    case SBI_BASE__GET_MVENDORID:
        return (sbi_ret_t){SBI_SUCCESS, RV_MVENDORID};
    case SBI_BASE__GET_MARCHID:
        return (sbi_ret_t){SBI_SUCCESS, RV_MARCHID};
    case SBI_BASE__GET_MIMPID:
        return (sbi_ret_t){SBI_SUCCESS, RV_MIMPID};
    case SBI_BASE__GET_SBI_SPEC_VERSION:
        return (sbi_ret_t){SBI_SUCCESS, (0UL << 24) | 3}; /* version 0.3 */
    case SBI_BASE__PROBE_EXTENSION: {
        int32_t eid = (int32_t) vm->x_regs[RV_R_A0];
        bool available =
            eid == SBI_EID_BASE || eid == SBI_EID_TIMER || eid == SBI_EID_RST;
        return (sbi_ret_t){SBI_SUCCESS, available};
    }
    default:
        return (sbi_ret_t){SBI_ERR_NOT_SUPPORTED, 0};
    }
}

#define SBI_HANDLE(TYPE) ret = handle_sbi_ecall_##TYPE(vm, vm->x_regs[RV_R_A6])

static void handle_sbi_ecall(vm_t *vm)
{
    sbi_ret_t ret;
    switch (vm->x_regs[RV_R_A7]) {
    case SBI_EID_BASE:
        SBI_HANDLE(BASE);
        break;
    case SBI_EID_TIMER:
        SBI_HANDLE(TIMER);
        break;
    case SBI_EID_RST:
        SBI_HANDLE(RST);
        break;
    default:
        ret = (sbi_ret_t){SBI_ERR_NOT_SUPPORTED, 0};
    }
    vm->x_regs[RV_R_A0] = (uint32_t) ret.error;
    vm->x_regs[RV_R_A1] = (uint32_t) ret.value;

    /* Clear error to allow execution to continue */
    vm->error = ERR_NONE;
}

#if !C64
struct mapper {
    char *addr;
    uint32_t size;
};

/* FIXME: Avoid hardcoding the capacity */
static struct mapper mapper[4] = {0};
static int map_index = 0;
static void unmap_files(void)
{
    while (map_index--) {
        if (!mapper[map_index].addr)
            continue;
        munmap(mapper[map_index].addr, mapper[map_index].size);
    }
}

static void map_file(char **ram_loc, const char *name)
{
    int fd = open(name, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "could not open %s\n", name);
        exit(2);
    }

    /* get file size */
    struct stat st;
    fstat(fd, &st);

    /* remap to a memory region */
    *ram_loc = mmap(*ram_loc, st.st_size, PROT_READ | PROT_WRITE,
                    MAP_FIXED | MAP_PRIVATE, fd, 0);
    if (*ram_loc == MAP_FAILED) {
        perror("mmap");
        close(fd);
        exit(2);
    }

    mapper[map_index].addr = *ram_loc;
    mapper[map_index].size = st.st_size;
    map_index++;

    /* The kernel selects a nearby page boundary and attempts to create
     * the mapping.
     */
    *ram_loc += st.st_size;

    close(fd);
}

static void usage(const char *execpath)
{
    fprintf(
        stderr,
        "Usage: %s -k linux-image [-b dtb] [-i initrd-image] [-d disk-image]\n",
        execpath);
}

static void handle_options(int argc,
                           char **argv,
                           char **kernel_file,
                           char **dtb_file,
                           char **initrd_file,
                           char **disk_file)
{
    *kernel_file = *dtb_file = *initrd_file = *disk_file = NULL;

    int optidx = 0;
    struct option opts[] = {
        {"kernel", 1, NULL, 'k'}, {"dtb", 1, NULL, 'b'},
        {"initrd", 1, NULL, 'i'}, {"disk", 1, NULL, 'd'},
        {"help", 0, NULL, 'h'},
    };

    int c;
    while ((c = getopt_long(argc, argv, "k:b:i:d:h", opts, &optidx)) != -1) {
        switch (c) {
        case 'k':
            *kernel_file = optarg;
            break;
        case 'b':
            *dtb_file = optarg;
            break;
        case 'i':
            *initrd_file = optarg;
            break;
        case 'd':
            *disk_file = optarg;
            break;
        case 'h':
            usage(argv[0]);
            exit(0);
        default:
            break;
        }
    }

    if (!*kernel_file) {
        fprintf(stderr,
                "Linux kernel image file must "
                "be provided via -k option.\n");
        usage(argv[0]);
        exit(2);
    }

    if (!*dtb_file)
        *dtb_file = "minimal.dtb";
}
#endif

emu_state_t emu;
vm_t vm = {
        .priv = &emu,
        .mem_fetch = mem_fetch,
        .mem_load = mem_load,
        .mem_store = mem_store
};

static void print_some_emu_state() {
    printf("PC: %lx\n", vm.pc);
    printf("TIMER LO, HI: %lx, %lx\n", emu.timer_lo, emu.timer_hi);
    printf("stopped: %d\n", emu.stopped);
    printf("UART: %d %d\n", emu.uart.in_ready, emu.uart.in_char);
    printf("PLIC: %lx %lx %lx %lx\n",
           emu.plic.masked,
           emu.plic.ip,
           emu.plic.ie,
           emu.plic.active);
}

#if C64
// FIXME: Load on-the fly from REU to avoid using all this stack/RAM at once
uint8_t reu_saved_state[250];
#endif

__attribute__((nonreentrant))
static int semu_start(int argc, char **argv)
{
#if C64
    (void) argc;
    (void) argv;
#else
    char *kernel_file;
    char *dtb_file;
    char *initrd_file;
    char *disk_file;
    handle_options(argc, argv, &kernel_file, &dtb_file, &initrd_file,
                   &disk_file);
    #endif

    /* Initialize the emulator */
    memset(&emu, 0, sizeof(emu));

#if C64
//    printf("c-64 semu risc-v emulator\n\r");
//    printf("git object id: $Id$\n");
//    printf("emu state begin: %p, size: %04x\n", &emu, sizeof(emu));
//    printf("vm state begin: %p, size: %04x\n", &vm, sizeof(vm));
#endif

    uint32_t dtb_addr = RAM_SIZE - INITRD_SIZE - DTB_SIZE; /* Device tree */
#if !C64
    /* Set up RAM */
    emu.ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (emu.ram == MAP_FAILED) {
        fprintf(stderr, "Could not map RAM\n");
        return 2;
    }
    memset(emu.ram, 0, RAM_SIZE);

    assert(!(((uintptr_t) emu.ram) & 0b11));

    /* *-----------------------------------------*
     * |              Memory layout              |
     * *----------------*----------------*-------*
     * |  kernel image  |  initrd image  |  dtb  |
     * *----------------*----------------*-------*
     */
    char *ram_loc = (char *) emu.ram;
    /* Load Linux kernel image */
    map_file(&ram_loc, kernel_file);
    /* Load at last 1 MiB to prevent kernel from overwriting it */
    ram_loc = ((char *) emu.ram) + dtb_addr;
    //map_file(&ram_loc, dtb_file);
    /* Load optional initrd image at last 8 MiB before the dtb region to
     * prevent kernel from overwritting it
     */
    /*
    if (initrd_file) {
        uint32_t initrd_addr = RAM_SIZE - INITRD_SIZE; // Init RAM disk
        ram_loc = ((char *) emu.ram) + initrd_addr;
        map_file(&ram_loc, initrd_file);
    }*/

    /* Hook for unmapping files */
    atexit(unmap_files);
#endif

    bool checkpoint_loaded = false;
    uint8_t *pbase=NULL;
#if C64
    load_from_reu(&reu_saved_state, PERSISTENCE_BASEADR, sizeof(reu_saved_state));

    pbase = reu_saved_state;
    checkpoint_loaded = load_all(&vm, &pbase);
//    printf("checkpoint loaded: %d\n", checkpoint_loaded);
//    printf("number of bytes deserialized: %d\n", pbase - reu_saved_state);
#else
    pbase = (uint8_t*)emu.ram + PERSISTENCE_BASEADR;
    checkpoint_loaded = load_all(&vm, &pbase);
//    printf("Checkpoint loaded: %d\n", checkpoint_loaded);
//    printf("Number of bytes deserialized: %ld\n", pbase - (uint8_t*)emu.ram - PERSISTENCE_BASEADR);
#endif

    if (!checkpoint_loaded) {
        /* Set up RISC-V hart */
        emu.timer_hi = emu.timer_lo = 0xFFFFFFFF;
        vm.page_table_addr = 0;
        vm.s_mode = true;
        vm.x_regs[RV_R_A0] = 0; /* hart ID. i.e., cpuid */
        vm.x_regs[RV_R_A1] = dtb_addr;
    }
    /* Set up peripherals */
    emu.uart.in_fd = 0, emu.uart.out_fd = 1;

#if !C64
    print_some_emu_state();
    capture_keyboard_input(); /* set up uart */
#if SEMU_HAS(VIRTIONET)
    if (!virtio_net_init(&(emu.vnet)))
        fprintf(stderr, "No virtio-net functioned\n");
    emu.vnet.ram = emu.ram;
#endif
#if SEMU_HAS(VIRTIOBLK)
    emu.vblk.ram = emu.ram;
    emu.disk = virtio_blk_init(&(emu.vblk), disk_file);
#endif
#endif

    /* Emulate */
    uint8_t peripheral_update_ctr = 0;
    while (!emu.stopped) {
        if (peripheral_update_ctr-- == 0) {

            u8250_check_ready(&emu.uart);
            if (emu.uart.in_ready)
                emu_update_uart_interrupts(&vm);

#if SEMU_HAS(VIRTIONET)
            virtio_net_refresh_queue(&emu.vnet);
            if (emu.vnet.InterruptStatus)
                emu_update_vnet_interrupts(&vm);
#endif

#if SEMU_HAS(VIRTIOBLK)
            if (emu.vblk.InterruptStatus)
                emu_update_vblk_interrupts(&vm);
#endif
            if (vm.insn_count_hi > emu.timer_hi ||
                (vm.insn_count_hi == emu.timer_hi && vm.insn_count > emu.timer_lo))
                vm.sip |= RV_INT_STI_BIT;
            else
                vm.sip &= ~RV_INT_STI_BIT;

            /* Stop after fixed amount of instructions for performance testing or
               to cross-check instruction traces etc. */
            //if (vm.insn_count > 200000000) exit(0);
        }

        vm_step(&vm);
        if (likely(!vm.error))
            continue;

        if (vm.error == ERR_EXCEPTION && vm.exc_cause == RV_EXC_ECALL_S) {
            handle_sbi_ecall(&vm);
            continue;
        }

        if (vm.error == ERR_EXCEPTION) {
            vm_trap(&vm);
            continue;
        }

        vm_error_report(&vm);
        return 2;
    }
    printf("\n\nVM RISCV insn count: %lu\n", (long unsigned)(vm.insn_count));
#if C64
    pbase = reu_saved_state;
    save_all(&vm, &pbase);
    // might seem meaningless to print here, but it shows up on kernalemu
    printf("number of bytes serialized: %d\n", pbase - reu_saved_state);
    save_to_reu(PERSISTENCE_BASEADR, &reu_saved_state, pbase - reu_saved_state);
    void (*reset_vect)() = (void*)0xfce2;
    reset_vect();
#else
    printf("Emulator stopped.\n");
    print_some_emu_state();
    printf("PC: %lx\n", vm.pc);
    pbase = (uint8_t*)emu.ram + PERSISTENCE_BASEADR;
    save_all(&vm, &pbase);
    printf("Number of bytes serialized: %ld\n", pbase - (uint8_t*)emu.ram - PERSISTENCE_BASEADR);
    FILE *reufile = fopen("reufile.semu.written", "wb");
    assert(reufile != NULL);
    int wreu = fwrite(emu.ram, 1, 16777216, reufile);
    printf("WROTE REU: %d\n",  wreu);

    // Note: This might fail if running repeatedly on the output file and it triggers another save.
    assert(wreu == 16777216);
    fclose(reufile);
#endif
    return 0;
}

__attribute__((nonreentrant))
int main(int argc, char **argv)
{
    return semu_start(argc, argv);
}
