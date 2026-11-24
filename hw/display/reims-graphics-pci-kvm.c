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

bool reims_graphics_kvm_pool_host_memory(void)
{
#if defined(__aarch64__)
    /*
     * The capability is the VM's (KVM_CHECK_EXTENSION on the VM's fd);
     * kvm_vm_check_extension is declared for per-target builds alone.
     */
    return kvm_enabled() &&
           kvm_vm_ioctl(kvm_state, KVM_CHECK_EXTENSION,
                        KVM_CAP_ARM_CACHEABLE_PFNMAP_SUPPORTED) <= 0;
#else
    return false;
#endif
}

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

/*
 * The time a call runs before the thread is signalled, which ends it with
 * its progress (-EINTR where it made none). A call retries a page with no
 * reload of its vCPU's MMU for as long as the VM's TDP root is obsolete,
 * until a signal (arch/x86/kvm/mmu/mmu.c:5063-5072, 4817-4843 in Linux
 * 7.3.3), holding KVM's SRCU, which every slot change then waits for under
 * the BQL. Roots go obsolete at a slot's delete where KVM zaps every slot
 * (KVM_X86_QUIRK_SLOT_ZAP_ALL on, KVM's default, which the device turns off
 * where KVM has it; mmu.c:7629-7650), at an MMIO generation wrap after many
 * slot changes (mmu.c:7653-7678), or when nx_huge_pages is switched
 * (mmu.c:7755-7766). The next call frees the obsolete root
 * (arch/x86/kvm/mmu.h:195-198). A slice takes 0.3-0.6 ms.
 */
#define REIMS_GRAPHICS_KVM_CALL_LIMIT_NS (20 * 1000 * 1000)

#ifndef sigev_notify_thread_id
#define sigev_notify_thread_id _sigev_un._tid
#endif

static struct {
    unsigned long id;            /* the helper's vCPU id */
    struct kvm_cpuid2 *cpuid;    /* vCPU 0's, for the helper */
    int tsc_khz;                 /* vCPU 0's, or <= 0 where KVM did not say */
    int fd;                      /* the helper's vCPU fd, -1 until made */
    pthread_t thread;            /* the populate thread */
    timer_t timer;               /* the populate thread's call limit */
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

/* On the populate thread: makes the helper and the call limit's timer. */
static bool reims_graphics_kvm_prefault_start(Error **errp)
{
    struct sigevent sev = {
        .sigev_notify = SIGEV_THREAD_ID,
        .sigev_signo = SIG_IPI,
    };
    sigset_t set;
    int fd, khz, ret;

    /*
     * The timer and a pause's kick signal this thread with SIG_IPI, whose
     * handler (kvm_ipi_signal, accel/kvm/kvm-all.c) does nothing on a
     * thread that is no vCPU's; the signal only ends the call in flight
     * (virt/kvm/kvm_main.c:4411-4415, arch/x86/kvm/mmu/mmu.c:5063-5065).
     */
    sigemptyset(&set);
    sigaddset(&set, SIG_IPI);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    reims_graphics_kvm_helper.thread = pthread_self();
    sev.sigev_notify_thread_id = qemu_get_thread_id();
    if (timer_create(CLOCK_MONOTONIC, &sev,
                     &reims_graphics_kvm_helper.timer) < 0) {
        error_setg_errno(errp, errno, "reims-graphics-pci: no pre-fault of "
                         "the pool: timer_create");
        return false;
    }

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

static int64_t reims_graphics_kvm_prefault_populate(hwaddr gpa, uint64_t size,
                                                    bool *cut)
{
    struct kvm_pre_fault_memory range = { .gpa = gpa, .size = size };
    struct itimerspec limit = {
        .it_value.tv_nsec = REIMS_GRAPHICS_KVM_CALL_LIMIT_NS,
    };
    struct itimerspec off = { 0 }, left;
    int ret, err;

    timer_settime(reims_graphics_kvm_helper.timer, 0, &limit, NULL);
    ret = ioctl(reims_graphics_kvm_helper.fd, KVM_PRE_FAULT_MEMORY, &range);
    err = errno;
    timer_settime(reims_graphics_kvm_helper.timer, 0, &off, &left);
    /* A one-shot timer with nothing left ran out: it signalled. */
    *cut = left.it_value.tv_sec == 0 && left.it_value.tv_nsec == 0;
    if (ret == 0) {
        /* Progress: KVM gives the range left back. */
        return range.size < size ? size - range.size : -EIO;
    }
    return -err;
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
