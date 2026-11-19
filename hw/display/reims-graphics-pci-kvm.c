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
#include "qemu/notify.h"
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
 * every call on it, and never runs: no KVM_RUN, no state put but its CPUID,
 * its TSC frequency and its TSC (below). KVM fills the VM's shared TDP
 * tables, whose root a vCPU picks by its role: CPUID's MAXPHYADDR (on a
 * host with 5-level paging), SMM and guest mode, and the memory type of a
 * mapping takes no vCPU state (arch/x86/kvm/mmu/mmu.c:5963-6010,
 * arch/x86/kvm/vmx/vmx.c:7791-7805 in Linux 7.3.3). Given vCPU 0's CPUID,
 * outside SMM and guest mode, the helper fills the tables the guest's vCPUs
 * use.
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
#define REIMS_GRAPHICS_KVM_MSR_IA32_TSC 0x10

static struct {
    unsigned long id;            /* the helper's vCPU id */
    struct kvm_cpuid2 *cpuid;    /* vCPU 0's, for the helper */
    int tsc_khz;                 /* vCPU 0's, or <= 0 where KVM did not say */
    int fd;                      /* the helper's vCPU fd, -1 until made */
    pthread_t thread;            /* the populate thread */
    Notifier tsc_put;
} reims_graphics_kvm_helper = { .fd = -1 };

/*
 * Writes the helper's TSC as the host, to 0, which KVM always takes as
 * synchronizing: the helper joins the TSC generation the VM's vCPUs are
 * in, with no change to theirs (arch/x86/kvm/msrs.c:1630-1632,
 * arch/x86/kvm/x86.c:1314-1319, 1351-1361), and a write to a helper
 * already in it changes nothing (x86.c:1287-1288).
 */
static int reims_graphics_kvm_helper_tsc_zero(int fd)
{
    struct {
        struct kvm_msrs info;
        struct kvm_msr_entry entries[1];
    } msrs = {
        .info.nmsrs = 1,
        .entries[0].index = REIMS_GRAPHICS_KVM_MSR_IA32_TSC,
    };

    return ioctl(fd, KVM_SET_MSRS, &msrs) == 1 ? 0 : -errno;
}

/*
 * After each put of a guest vCPU's TSC at a reset (a guest reboot writes
 * TSC 1 on each vCPU, target/i386/cpu.c:9486-9496) or of its full state: on
 * that vCPU's thread. KVM's master clock needs every vCPU, the helper
 * included, in one TSC generation (x86.c:1088-1101); the first vCPU's
 * write starts a new one, which the others join, and a helper nobody
 * wrote would be left out, so the master clock would go off for the guest's
 * kvmclock until the next reset (x86.c:1587-1594). The helper joins after
 * each put, so it is in the generation the last put leaves.
 */
static void reims_graphics_kvm_tsc_put(Notifier *n, void *data)
{
    CPUState *cpu = data;
    int fd = qatomic_read(&reims_graphics_kvm_helper.fd);
    int ret;

    if (fd < 0) {
        return;
    }
    ret = reims_graphics_kvm_helper_tsc_zero(fd);
    trace_reims_graphics_prefault_tsc_rejoin(cpu->cpu_index, ret);
    if (ret < 0) {
        warn_report_once("reims-graphics-pci: the pre-fault helper's TSC "
                         "write failed (%s): KVM's master clock may stay "
                         "off", strerror(-ret));
    }
}

/* On the populate thread: makes the helper. */
static bool reims_graphics_kvm_prefault_start(Error **errp)
{
    sigset_t set;
    int fd, khz, ret;

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
    /*
     * The helper's TSC frequency is the guest's vCPUs', as KVM's TSC
     * generations match only at one frequency; then it joins theirs, and
     * from here on rejoins it after each of their puts, whatever comes
     * of the rest: the helper is in the VM for good.
     */
    khz = reims_graphics_kvm_helper.tsc_khz;
    if (khz > 0 && ioctl(fd, KVM_GET_TSC_KHZ) != khz &&
        ioctl(fd, KVM_SET_TSC_KHZ, khz) < 0) {
        error_setg_errno(errp, errno, "reims-graphics-pci: no pre-fault of "
                         "the pool: KVM_SET_TSC_KHZ %d on its vCPU", khz);
        return false;
    }
    ret = reims_graphics_kvm_helper_tsc_zero(fd);
    if (ret < 0) {
        error_setg_errno(errp, -ret, "reims-graphics-pci: no pre-fault of "
                         "the pool: the TSC write on its vCPU");
        return false;
    }
    qatomic_set(&reims_graphics_kvm_helper.fd, fd);
    if (ioctl(fd, KVM_SET_CPUID2, reims_graphics_kvm_helper.cpuid) < 0) {
        error_setg_errno(errp, errno, "reims-graphics-pci: no pre-fault of "
                         "the pool: KVM_SET_CPUID2 on its vCPU");
        return false;
    }
    trace_reims_graphics_prefault_helper(reims_graphics_kvm_helper.id,
                                         reims_graphics_kvm_helper.cpuid->nent,
                                         khz);
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
    reims_graphics_kvm_helper.tsc_khz = ioctl(cpu->kvm_fd, KVM_GET_TSC_KHZ);
    reims_graphics_kvm_helper.tsc_put.notify = reims_graphics_kvm_tsc_put;
    kvm_tsc_put_add_notifier(&reims_graphics_kvm_helper.tsc_put);
    return &reims_graphics_kvm_prefault;
}

#else

const ReimsGraphicsPrefaultOps *reims_graphics_kvm_prefault_ops(void)
{
    return NULL;
}

#endif
