/*
 * User Thread Control Block (UTCB)
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * Copyright (C) 2012 Udo Steinberg, Intel Corporation.
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

#pragma once

#include "buddy.h"
#include "crd.h"
#include "util.h"

class Cpu_regs;

class Utcb_segment
{
    public:
        uint16  sel, ar;
        uint32  limit;
        uint64  base;

        ALWAYS_INLINE
        inline void set_vmx (mword s, mword b, mword l, mword a)
        {
            sel   = static_cast<uint16>(s);
            ar    = static_cast<uint16>((a >> 4 & 0x1f00) | (a & 0xff));
            limit = static_cast<uint32>(l);
            base  = b;
        }
};

class Utcb_head
{
    protected:
		uint16  top;
		uint16  bottom;
        mword   items;
        Crd     xlt, del;
        mword   tls;
};

class Utcb_data
{
    protected:
        union {
            struct {
                mword           mtd, inst_len, rip, rflags;
                uint32          intr_state, actv_state;
                union {
                    struct {
                        uint32  intr_info, intr_error;
                    };
                    uint64      inj;
                };

                mword           rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi;
#ifdef __x86_64__
                mword           r8,  r9,  r10, r11, r12, r13, r14, r15;
#endif
                uint64          qual[2];
                uint32          ctrl[2];
                uint64          reserved;
                mword           cr0, cr2, cr3, cr4;
#ifdef __x86_64__
                mword           cr8, efer;
#endif
                mword           dr7, sysenter_cs, sysenter_rsp, sysenter_rip;
                Utcb_segment    es, cs, ss, ds, fs, gs, ld, tr, gd, id;
                uint64          tsc_val, tsc_off;
            };

            mword mr[];
        };
};

class Utcb : public Utcb_head, private Utcb_data
{
    private:
        static mword const words = (PAGE_SIZE - sizeof (Utcb_head)) / sizeof (mword);

        // we use a power of two for performance reasons. Limiting it to 1024-1 words is not sufficient
        // because then there's only one word left and we need 4 for the header. Thus, we use 512-1
        // as a limit for bottom. For top we use a even lower limit to ensure that top + bottom <= words
        // This wastes a bit of space, but it should still be sufficient because it is not expected
        // to have many frames in the utcb. It shouldn't be much more than 3 in most cases.
        inline mword boff() const { return bottom & ((PAGE_SIZE / (2 * sizeof(mword))) - 1); }
		inline mword toff() const { return top & ((PAGE_SIZE / (4 * sizeof(mword))) - 1); }
        inline Utcb *cur_frame() {
        	return reinterpret_cast<Utcb*>(reinterpret_cast<mword*>(this) + boff());
        }
        inline const Utcb *cur_frame() const {
        	return reinterpret_cast<const Utcb*>(reinterpret_cast<const mword*>(this) + boff());
        }
        ALWAYS_INLINE
        inline mword maxui() const { return (words - (boff() + toff())) / 1; }

    public:
        void load_exc (Cpu_regs *);
        void load_vmx (Cpu_regs *);
        void load_svm (Cpu_regs *);
        void save_exc (Cpu_regs *);
        void save_vmx (Cpu_regs *);
        void save_svm (Cpu_regs *);

        inline Crd translate() { return cur_frame()->xlt; }
        inline Crd delegate() { return cur_frame()->del; }

        inline mword ucnt() const { return static_cast<uint16>(cur_frame()->items); }
        inline mword tcnt() const { return static_cast<uint16>(cur_frame()->items >> 16); }

        inline mword ti() const { return min ((words - (boff() + toff())) / 2, tcnt()); }
        ALWAYS_INLINE
        inline mword ui() const { return min (maxui(), ucnt()); }

        ALWAYS_INLINE NONNULL
        inline void save (Utcb *dst)
        {
            register mword n = min(dst->maxui(),ui());
            const Utcb *thiz = cur_frame();

            dst = dst->cur_frame();
            dst->items = thiz->items;
#if 0
            mword *d = dst->mr, *s = mr;
            asm volatile ("rep; movsl" : "+D" (d), "+S" (s), "+c" (n) : : "memory");
#else
            for (unsigned long i = 0; i < n; i++)
                dst->mr[i] = thiz->mr[i];
#endif
        }

        ALWAYS_INLINE
        inline Xfer *xfer() { return reinterpret_cast<Xfer *>(this) + PAGE_SIZE / sizeof (Xfer) - (1 + toff() / 2); }

        ALWAYS_INLINE
        static inline void *operator new (size_t) { return Buddy::allocator.alloc (0, Buddy::FILL_0); }

        ALWAYS_INLINE
        static inline void operator delete (void *ptr) { Buddy::allocator.free (reinterpret_cast<mword>(ptr)); }
};
