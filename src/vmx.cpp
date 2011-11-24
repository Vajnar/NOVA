/*
 * Virtual Machine Extensions (VMX)
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * This file is part of the NOVA microhypervisor.
 *
 * NOVA is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * NOVA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License version 2 for more details.
 */

#include "bits.h"
#include "cmdline.h"
#include "ept.h"
#include "gdt.h"
#include "hip.h"
#include "idt.h"
#include "msr.h"
#include "tss.h"
#include "util.h"
#include "vmx.h"
#include "x86.h"

Vmcs *              Vmcs::current;
unsigned            Vmcs::vpid_ctr;
Vmcs::vmx_basic     Vmcs::basic;
Vmcs::vmx_ept_vpid  Vmcs::ept_vpid;
Vmcs::vmx_ctrl_pin  Vmcs::ctrl_pin;
Vmcs::vmx_ctrl_cpu  Vmcs::ctrl_cpu[2];
Vmcs::vmx_ctrl_exi  Vmcs::ctrl_exi;
Vmcs::vmx_ctrl_ent  Vmcs::ctrl_ent;
mword               Vmcs::fix_cr0_set, Vmcs::fix_cr0_clr;
mword               Vmcs::fix_cr4_set, Vmcs::fix_cr4_clr;

Vmcs::Vmcs (mword esp, mword bmp, mword cr3, uint64 eptp) : rev (basic.revision)
{
    clear();

    make_current();

    uint32 pin = PIN_EXTINT | PIN_NMI | PIN_VIRT_NMI;
    pin |= ctrl_pin.set;
    pin &= ctrl_pin.clr;
    write (PIN_EXEC_CTRL, pin);

    uint32 exi = EXI_INTA;
    exi |= ctrl_exi.set;
    exi &= ctrl_exi.clr;
    write (EXI_CONTROLS, exi);

    uint32 ent = 0;
    ent |= ctrl_ent.set;
    ent &= ctrl_ent.clr;
    write (ENT_CONTROLS, ent);

    write (PF_ERROR_MASK, 0);
    write (PF_ERROR_MATCH, 0);
    write (CR3_TARGET_COUNT, 0);

    write (VMCS_LINK_PTR,    ~0ul);
    write (VMCS_LINK_PTR_HI, ~0ul);

    write (VPID, ++vpid_ctr);

    write (EPTP,    static_cast<mword>(eptp) | (Ept::max() - 1) << 3 | 6);
    write (EPTP_HI, static_cast<mword>(eptp >> 32));

    write (IO_BITMAP_A, bmp);
    write (IO_BITMAP_B, bmp + PAGE_SIZE);

    write (HOST_SEL_CS, SEL_KERN_CODE);
    write (HOST_SEL_SS, SEL_KERN_DATA);
    write (HOST_SEL_DS, SEL_KERN_DATA);
    write (HOST_SEL_ES, SEL_KERN_DATA);
    write (HOST_SEL_TR, SEL_TSS_RUN);

    write (HOST_CR3, cr3);
    write (HOST_CR0, get_cr0() | Cpu::CR0_TS);
    write (HOST_CR4, get_cr4());

    write (HOST_BASE_TR,   reinterpret_cast<mword>(&Tss::run));
    write (HOST_BASE_GDTR, reinterpret_cast<mword>(Gdt::gdt));
    write (HOST_BASE_IDTR, reinterpret_cast<mword>(Idt::idt));

    write (HOST_SYSENTER_CS,  SEL_KERN_CODE);
    write (HOST_SYSENTER_ESP, reinterpret_cast<mword>(&Tss::run.sp0));
    write (HOST_SYSENTER_EIP, reinterpret_cast<mword>(&entry_sysenter));

    write (HOST_RSP, esp);
    write (HOST_RIP, reinterpret_cast<mword>(&entry_vmx));
}

void Vmcs::init()
{
    if (!Cpu::feature (Cpu::FEAT_VMX) || (Msr::read<uint32>(Msr::IA32_FEATURE_CONTROL) & 0x5) != 0x5) {
        Hip::remove (Hip::FEAT_VMX);
        return;
    }

    fix_cr0_set =  Msr::read<mword>(Msr::IA32_VMX_CR0_FIXED0);
    fix_cr0_clr = ~Msr::read<mword>(Msr::IA32_VMX_CR0_FIXED1);
    fix_cr4_set =  Msr::read<mword>(Msr::IA32_VMX_CR4_FIXED0);
    fix_cr4_clr = ~Msr::read<mword>(Msr::IA32_VMX_CR4_FIXED1);

    basic.val       = Msr::read<uint64>(Msr::IA32_VMX_BASIC);
    ctrl_exi.val    = Msr::read<uint64>(basic.ctrl ? Msr::IA32_VMX_TRUE_EXIT  : Msr::IA32_VMX_CTRL_EXIT);
    ctrl_ent.val    = Msr::read<uint64>(basic.ctrl ? Msr::IA32_VMX_TRUE_ENTRY : Msr::IA32_VMX_CTRL_ENTRY);
    ctrl_pin.val    = Msr::read<uint64>(basic.ctrl ? Msr::IA32_VMX_TRUE_PIN   : Msr::IA32_VMX_CTRL_PIN);
    ctrl_cpu[0].val = Msr::read<uint64>(basic.ctrl ? Msr::IA32_VMX_TRUE_CPU0  : Msr::IA32_VMX_CTRL_CPU0);

    if (has_secondary())
        ctrl_cpu[1].val = Msr::read<uint64>(Msr::IA32_VMX_CTRL_CPU1);
    if (has_ept() || has_vpid())
        ept_vpid.val = Msr::read<uint64>(Msr::IA32_VMX_EPT_VPID);
    if (has_ept())
        Ept::ord = min (Ept::ord, static_cast<mword>(bit_scan_reverse (static_cast<mword>(ept_vpid.super)) + 2) * Ept::bpl() - 1);
    if (has_urg())
        fix_cr0_set &= ~(Cpu::CR0_PG | Cpu::CR0_PE);

    ctrl_cpu[0].set |= CPU_HLT | CPU_IO | CPU_IO_BITMAP | CPU_SECONDARY;
    ctrl_cpu[1].set |= CPU_VPID | CPU_URG;

    if (Cmdline::vtlb || !ept_vpid.invept)
        ctrl_cpu[1].clr &= ~(CPU_EPT | CPU_URG);
    if (Cmdline::novpid || !ept_vpid.invvpid)
        ctrl_cpu[1].clr &= ~CPU_VPID;

    set_cr0 ((get_cr0() & ~fix_cr0_clr) | fix_cr0_set);
    set_cr4 ((get_cr4() & ~fix_cr4_clr) | fix_cr4_set);

    Vmcs *root = new Vmcs;

    trace (TRACE_VMX, "VMCS:%#010lx REV:%#x EPT:%u VPID:%u UG:%u", Buddy::ptr_to_phys (root), basic.revision, has_ept(), has_vpid(), has_urg());
}
