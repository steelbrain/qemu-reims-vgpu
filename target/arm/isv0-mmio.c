/*
 * Emulation of loads and stores to MMIO that report no instruction syndrome
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "system/hw_accel.h"
#include "system/memory.h"
#include "cpu.h"
#include "internals.h"

/*
 * Emulate a load/store that took a data abort with ISV=0, i.e. with no
 * instruction syndrome: the CPU only describes single general-purpose
 * loads/stores in the ISS, so LDP/STP and SIMD&FP accesses to MMIO arrive
 * undecoded. macOS 13+ guest kernels perform such accesses against device
 * memory during early boot (the macOS 12 kernel does not), so vmapple needs
 * this to run newer guests.
 *
 * Decodes the faulting instruction from guest memory and performs the access
 * at the already-translated fault IPA. Covers exactly the A64 classes that
 * report ISV=0 and can plausibly target MMIO:
 *   - load/store pair, GPR and SIMD&FP (LDP/STP/LDNP/STNP/LDPSW), all
 *     indexing modes
 *   - load/store single GPR, pre/post-index and register-offset forms
 *   - load/store single SIMD&FP register (LDR/STR/LDUR/STUR B/H/S/D/Q),
 *     immediate/register-offset/pre/post-index forms
 * Anything else (exclusives, atomics, SP-based writeback, ...) returns false
 * so the caller can fail loudly with full context.
 *
 * With @strict, every access is made before any register changes, and the
 * function returns false, changing no register, if one of them fails: the
 * address has no device in QEMU's address space, as for an interrupt
 * controller that the accelerator emulates itself. Without @strict a failed
 * access is ignored, as a bus would read zeros and drop writes.
 *
 * On success the emulated instruction is complete: registers and PC are
 * updated in env, whose state was pulled with cpu_synchronize_state(), so
 * the accelerator pushes it back before the vCPU runs again.
 */
bool arm_emulate_isv0_mmio(CPUState *cpu, hwaddr ipa, bool strict,
                           uint32_t *insn_out)
{
    CPUARMState *env = cpu_env(cpu);
    AddressSpace *as = cpu_get_address_space(cpu, ARMASIdx_NS);
    MemTxResult res = MEMTX_OK;
    uint32_t insn = 0;

    cpu_synchronize_state(cpu);

    if (cpu_memory_rw_debug(cpu, env->pc, &insn, sizeof(insn), false)) {
        return false;
    }
    *insn_out = insn;

    if ((insn & 0x38000000) == 0x28000000) {
        /* Load/store pair (incl. no-allocate): C4.1.66 op0=x101. */
        uint32_t opc = extract32(insn, 30, 2);
        bool is_vector = extract32(insn, 26, 1);
        /* 0=STNP/LDNP 1=post 2=offset 3=pre */
        uint32_t mode = extract32(insn, 23, 2);
        bool is_load = extract32(insn, 22, 1);
        int64_t imm7 = sextract32(insn, 15, 7);
        uint32_t rt2 = extract32(insn, 10, 5);
        uint32_t rn = extract32(insn, 5, 5);
        uint32_t rt = extract32(insn, 0, 5);
        bool wback = (mode == 1) || (mode == 3);
        bool sext = false;
        uint8_t buf[2][16] = { 0 };
        int esize;

        if (is_vector) {
            if (opc == 3) {
                return false;               /* unallocated */
            }
            esize = 4 << opc;               /* S/D/Q */
        } else if (opc == 0) {
            esize = 4;
        } else if (opc == 1 && is_load) {
            esize = 4;
            sext = true;                    /* LDPSW */
        } else if (opc == 2) {
            esize = 8;
        } else {
            return false;                   /* unallocated / STGP */
        }
        if (wback && rn == 31) {
            return false;               /* SP-based MMIO writeback: refuse */
        }

        if (is_load) {
            for (int i = 0; i < 2; i++) {
                res |= address_space_read(as, ipa + (hwaddr)i * esize,
                                          MEMTXATTRS_UNSPECIFIED, buf[i],
                                          esize);
            }
            if (strict && res != MEMTX_OK) {
                return false;
            }
            for (int i = 0; i < 2; i++) {
                uint32_t reg = i ? rt2 : rt;

                if (is_vector) {
                    env->vfp.zregs[reg].d[0] = ldq_le_p(buf[i]);
                    env->vfp.zregs[reg].d[1] =
                        esize == 16 ? ldq_le_p(buf[i] + 8) : 0;
                } else if (reg != 31) {
                    uint64_t v = esize == 4 ? ldl_le_p(buf[i])
                                            : ldq_le_p(buf[i]);
                    env->xregs[reg] = sext ? sextract64(v, 0, 32) : v;
                }
            }
        } else {
            for (int i = 0; i < 2; i++) {
                uint32_t reg = i ? rt2 : rt;

                if (is_vector) {
                    stq_le_p(buf[i], env->vfp.zregs[reg].d[0]);
                    if (esize == 16) {
                        stq_le_p(buf[i] + 8, env->vfp.zregs[reg].d[1]);
                    }
                } else if (reg != 31) {
                    if (esize == 4) {
                        stl_le_p(buf[i], (uint32_t)env->xregs[reg]);
                    } else {
                        stq_le_p(buf[i], env->xregs[reg]);
                    }
                }
                res |= address_space_write(as, ipa + (hwaddr)i * esize,
                                           MEMTXATTRS_UNSPECIFIED, buf[i],
                                           esize);
            }
            if (strict && res != MEMTX_OK) {
                return false;
            }
        }
        if (wback) {
            env->xregs[rn] += imm7 * esize;
        }
        env->pc += 4;
        return true;
    }

    if (extract32(insn, 24, 6) == 0x38) {
        /*
         * Load/store single GPR, imm9/register-offset forms: bits[29:24]=
         * 111000. The pre/post-indexed (writeback) forms report ISV=0
         * because the syndrome cannot describe the base update: macOS 13+
         * uses e.g. "str w9, [x8, #128]!" on MMIO.
         */
        uint32_t size = extract32(insn, 30, 2);
        uint32_t opc = extract32(insn, 22, 2);
        uint32_t rn = extract32(insn, 5, 5);
        uint32_t rt = extract32(insn, 0, 5);
        bool wback = false;
        int esize = 1 << size;
        uint8_t buf[8] = { 0 };

        if (extract32(insn, 21, 1)) {
            if (extract32(insn, 10, 2) != 2) {
                return false;           /* atomics etc., not register-offset */
            }
        } else {
            switch (extract32(insn, 10, 2)) {
            case 0:                     /* LDUR/STUR: no writeback */
            case 2:                     /* LDTR/STTR: treat as plain access */
                break;
            case 1:                     /* post-index */
            case 3:                     /* pre-index */
                wback = true;
                break;
            }
        }
        if (size == 3 && opc == 2) {
            return false;               /* PRFUM: prefetch never aborts */
        }
        if (wback && rn == 31) {
            return false;               /* SP-based MMIO writeback: refuse */
        }

        if (opc == 0) {                 /* store */
            if (rt != 31) {
                stq_le_p(buf, env->xregs[rt]);
            }
            res = address_space_write(as, ipa, MEMTXATTRS_UNSPECIFIED, buf,
                                      esize);
            if (strict && res != MEMTX_OK) {
                return false;
            }
        } else {
            /* load: 01 zext, 10 sext to X, 11 sext to W */
            uint64_t v;

            res = address_space_read(as, ipa, MEMTXATTRS_UNSPECIFIED, buf,
                                     esize);
            if (strict && res != MEMTX_OK) {
                return false;
            }
            v = ldq_le_p(buf) & MAKE_64BIT_MASK(0, esize * 8);
            if (opc == 2) {
                v = sextract64(v, 0, esize * 8);
            } else if (opc == 3) {
                v = (uint32_t)sextract64(v, 0, esize * 8);
            }
            if (rt != 31) {
                env->xregs[rt] = v;
            }
        }
        if (wback) {
            env->xregs[rn] += sextract32(insn, 12, 9);
        }
        env->pc += 4;
        return true;
    }

    if (extract32(insn, 24, 6) == 0x3d || extract32(insn, 24, 6) == 0x3c) {
        /* Load/store single SIMD&FP register: bits[29:24]=1111 0V. */
        uint32_t size = extract32(insn, 30, 2);
        uint32_t opc = extract32(insn, 22, 2);
        bool unsigned_imm = extract32(insn, 24, 2) == 1;
        bool is_load = opc & 1;
        uint32_t rn = extract32(insn, 5, 5);
        uint32_t rt = extract32(insn, 0, 5);
        uint32_t scale = size;
        bool wback = false;
        int esize;
        uint8_t buf[16] = { 0 };

        if (opc & 2) {
            if (size != 0) {
                return false;               /* only Q extends the size range */
            }
            scale = 4;
        }
        esize = 1 << scale;

        if (!unsigned_imm) {
            if (extract32(insn, 21, 1)) {
                if (extract32(insn, 10, 2) != 2) {
                    return false;           /* not a register-offset form */
                }
            } else {
                switch (extract32(insn, 10, 2)) {
                case 0:                     /* LDUR/STUR: no writeback */
                    break;
                case 1:                     /* post-index */
                case 3:                     /* pre-index */
                    wback = true;
                    break;
                default:
                    return false;
                }
            }
        }
        if (wback && rn == 31) {
            return false;               /* SP-based MMIO writeback: refuse */
        }

        if (is_load) {
            res = address_space_read(as, ipa, MEMTXATTRS_UNSPECIFIED, buf,
                                     esize);
            if (strict && res != MEMTX_OK) {
                return false;
            }
            env->vfp.zregs[rt].d[0] = ldq_le_p(buf);
            env->vfp.zregs[rt].d[1] = esize == 16 ? ldq_le_p(buf + 8) : 0;
        } else {
            stq_le_p(buf, env->vfp.zregs[rt].d[0]);
            if (esize == 16) {
                stq_le_p(buf + 8, env->vfp.zregs[rt].d[1]);
            }
            res = address_space_write(as, ipa, MEMTXATTRS_UNSPECIFIED, buf,
                                      esize);
            if (strict && res != MEMTX_OK) {
                return false;
            }
        }
        if (wback) {
            env->xregs[rn] += sextract32(insn, 12, 9);
        }
        env->pc += 4;
        return true;
    }

    return false;
}
