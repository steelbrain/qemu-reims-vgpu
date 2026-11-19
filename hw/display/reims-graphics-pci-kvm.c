/*
 * QEMU reims-graphics device: its calls into KVM, for the targets with KVM
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <sys/ioctl.h>
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "system/kvm.h"
#include <linux/kvm.h>
#include "reims-graphics-pci-kvm.h"
#include "reims-graphics-pci-prefault.h"
#include "trace.h"

bool reims_graphics_kvm_disable_slot_zap_all(void)
{
#ifdef KVM_X86_QUIRK_SLOT_ZAP_ALL
    struct kvm_enable_cap cap = {
        .cap = KVM_CAP_DISABLE_QUIRKS2,
        .args = { KVM_X86_QUIRK_SLOT_ZAP_ALL },
    };

    if (!kvm_enabled() ||
        !(kvm_check_extension(kvm_state, KVM_CAP_DISABLE_QUIRKS2) &
          KVM_X86_QUIRK_SLOT_ZAP_ALL)) {
        return true;
    }
    return kvm_vm_ioctl(kvm_state, KVM_ENABLE_CAP, &cap) >= 0;
#else
    return true;
#endif
}

#if defined(__x86_64__) && defined(KVM_PRE_FAULT_MEMORY)

/*
 * The pool's populate on KVM: KVM_PRE_FAULT_MEMORY, a vCPU ioctl, on a vCPU
 * of the populate thread's own, the helper. The guest's vCPUs are never
 * used for it: each call holds its vCPU's mutex, which a guest vCPU's
 * KVM_RUN holds while the guest runs.
 *
 * The helper is made by the populate thread (KVM_CREATE_VCPU), which issues
 * every call on it, and never runs: no KVM_RUN, no state put but its CPUID.
 * KVM fills the VM's shared TDP tables, whose root a vCPU picks by its
 * role: CPUID's MAXPHYADDR (on a host with 5-level paging), SMM and guest
 * mode, and the memory type of a mapping takes no vCPU state
 * (arch/x86/kvm/mmu/mmu.c:5963-6010, arch/x86/kvm/vmx/vmx.c:7791-7805 in
 * Linux 7.3.3). Given vCPU 0's CPUID, outside SMM and guest mode, the
 * helper fills the tables the guest's vCPUs use.
 *
 * Its id is one past every APIC id the guest has or can be given
 * (MachineClass::possible_cpu_arch_ids), so no message the guest sends to
 * the CPUs it is told of names it, and its APIC stays software-disabled
 * with its LVTs masked, so a fixed, lowest-priority or logical message
 * never takes it. KVM_CAP_MAX_VCPU_ID is one higher for it
 * (target/i386/kvm/kvm.c, kvm_set_max_apic_id). Ids past 254 make none:
 * AVIC without x2AVIC inhibits itself for the whole VM at such an id.
 */

#define REIMS_GRAPHICS_KVM_HELPER_MAX_ID 254
#define REIMS_GRAPHICS_KVM_CPUID_MAX 4096

static struct {
    unsigned long id;            /* the helper's vCPU id */
    struct kvm_cpuid2 *cpuid;    /* vCPU 0's, for the helper */
    int fd;                      /* the helper's vCPU fd, -1 until made */
    pthread_t thread;            /* the populate thread */
} reims_graphics_kvm_helper = { .fd = -1 };

/* On the populate thread: makes the helper. */
static bool reims_graphics_kvm_prefault_start(Error **errp)
{
    sigset_t set;
    int fd;

    /*
     * A pause's kick signals this thread with SIG_IPI, whose handler
     * (kvm_ipi_signal, accel/kvm/kvm-all.c) does nothing on a thread that
     * is no vCPU's; the signal only ends the call in flight
     * (virt/kvm/kvm_main.c:4411-4415, arch/x86/kvm/mmu/mmu.c:5063-5065).
     */
    sigemptyset(&set);
    sigaddset(&set, SIG_IPI);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    reims_graphics_kvm_helper.thread = pthread_self();

    fd = kvm_vm_ioctl(kvm_state, KVM_CREATE_VCPU,
                      (void *)reims_graphics_kvm_helper.id);
    if (fd < 0) {
        error_setg_errno(errp, -fd, "reims-graphics-pci: no pre-fault of the "
                         "pool: KVM_CREATE_VCPU %lu",
                         reims_graphics_kvm_helper.id);
        return false;
    }
    reims_graphics_kvm_helper.fd = fd;
    if (ioctl(fd, KVM_SET_CPUID2, reims_graphics_kvm_helper.cpuid) < 0) {
        error_setg_errno(errp, errno, "reims-graphics-pci: no pre-fault of "
                         "the pool: KVM_SET_CPUID2 on its vCPU");
        return false;
    }
    trace_reims_graphics_prefault_helper(reims_graphics_kvm_helper.id,
                                         reims_graphics_kvm_helper.cpuid->nent);
    return true;
}

static int64_t reims_graphics_kvm_prefault_populate(hwaddr gpa, uint64_t size)
{
    struct kvm_pre_fault_memory range = { .gpa = gpa, .size = size };
    int ret;

    ret = ioctl(reims_graphics_kvm_helper.fd, KVM_PRE_FAULT_MEMORY, &range);
    if (ret == 0) {
        /* Progress: KVM gives the range left back. */
        return range.size < size ? size - range.size : -EIO;
    }
    return -errno;
}

static void reims_graphics_kvm_prefault_kick(void)
{
    pthread_kill(reims_graphics_kvm_helper.thread, SIG_IPI);
}

static const ReimsGraphicsPrefaultOps reims_graphics_kvm_prefault = {
    .start = reims_graphics_kvm_prefault_start,
    .populate = reims_graphics_kvm_prefault_populate,
    .kick = reims_graphics_kvm_prefault_kick,
};

/* vCPU 0's CPUID, as KVM holds it: NULL where it did not answer. */
static struct kvm_cpuid2 *reims_graphics_kvm_cpuid(CPUState *cpu)
{
    struct kvm_cpuid2 *cpuid;
    unsigned nent;

    for (nent = 128; nent <= REIMS_GRAPHICS_KVM_CPUID_MAX; nent *= 2) {
        cpuid = g_malloc0(sizeof(*cpuid) +
                          nent * sizeof(struct kvm_cpuid_entry2));
        cpuid->nent = nent;
        if (ioctl(cpu->kvm_fd, KVM_GET_CPUID2, cpuid) == 0) {
            return cpuid;
        }
        g_free(cpuid);
        if (errno != E2BIG) {
            break;
        }
    }
    return NULL;
}

const ReimsGraphicsPrefaultOps *reims_graphics_kvm_prefault_ops(void)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    MachineClass *mc = MACHINE_GET_CLASS(ms);
    const CPUArchIdList *ids;
    CPUState *cpu = first_cpu;
    unsigned long id = 0;
    int i;

    if (!kvm_enabled() || !kvm_pre_fault_memory_supported) {
        return NULL;
    }
    if (!mc->possible_cpu_arch_ids || !cpu || cpu->kvm_fd < 0) {
        warn_report("reims-graphics-pci: no pre-fault of the pool: the "
                    "machine names no vCPUs");
        return NULL;
    }
    ids = mc->possible_cpu_arch_ids(ms);
    for (i = 0; i < ids->len; i++) {
        id = MAX(id, ids->cpus[i].arch_id + 1);
    }
    if (id > REIMS_GRAPHICS_KVM_HELPER_MAX_ID) {
        warn_report("reims-graphics-pci: no pre-fault of the pool: its "
                    "vCPU's id would be %lu, past %d", id,
                    REIMS_GRAPHICS_KVM_HELPER_MAX_ID);
        return NULL;
    }
    reims_graphics_kvm_helper.id = id;
    reims_graphics_kvm_helper.cpuid = reims_graphics_kvm_cpuid(cpu);
    if (!reims_graphics_kvm_helper.cpuid) {
        warn_report("reims-graphics-pci: no pre-fault of the pool: "
                    "KVM_GET_CPUID2 on vCPU 0: %s", strerror(errno));
        return NULL;
    }
    return &reims_graphics_kvm_prefault;
}

#else

const ReimsGraphicsPrefaultOps *reims_graphics_kvm_prefault_ops(void)
{
    return NULL;
}

#endif
