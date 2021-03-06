//
// =========================================================
// x86 Hardware Assisted Virtualization Demo for AMD-V (SVM)
// =========================================================
//
// Description: A very basic driver and associated user app
// that walks through all the steps to do a successful vmrun.
// After vmrun, the guest code does a vmmcall and #vmexits
// back to the host. The guest state mirrors the host.
//
// References for study:
//
// 1. AMD64 Architecture Programmer's Manual
//    (Vol 2: System Programming)
//
// 2. KVM from the Linux kernel
//    (Mostly kvm_main.c, mmu.c, x86.c svm.c)
//
// 3. Original Intel VT-x vmlaunch demo
//    (https://github.com/vishmohan/vmlaunch)
//
// 4. Original kvmsample demo
//    (https://github.com/soulxu/kvmsample)
//
// Copyright (C) 2017 STROMASYS SA <http://www.stromasys.com>
// Copyright (C) 2006 Qumranet, Inc.
//
// Authors:
//
// - Elias Kouskoumvekakis <eliask.kousk@stromasys.com>
// - Avi Kivity            <avi@qumranet.com>   (KVM)
// - Yaniv Kamay           <yaniv@qumranet.com> (KVM)
//
// This work is licensed under the terms of the GNU GPL, version 2.
// See the LICENSE file in the top-level directory.
//

#ifndef VMRUN_H
#define VMRUN_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/bug.h>
#include <linux/mm.h>
#include <linux/mmu_notifier.h>
#include <linux/preempt.h>

#include "page_track.h"

#define CPUID_EXT_1_SVM_LEAF      0x80000001
#define CPUID_EXT_1_SVM_BIT       0x2
#define CPUID_EXT_A_SVM_LOCK_LEAF 0x8000000a
#define CPUID_EXT_A_SVM_LOCK_BIT  0x2

#define MSR_VM_CR_SVM_DIS_ADDR    0xc0010114
#define MSR_VM_CR_SVM_DIS_BIT     0x4
#define MSR_EFER_SVM_EN_ADDR      0xc0000080
#define MSR_EFER_SVM_EN_BIT       0xC
#define MSR_VM_HSAVE_PA           0xc0010117

#define HF_GIF_MASK		  (1 << 0)
#define HF_GUEST_MASK		  (1 << 5) /* VCPU is in guest-mode */
#define HF_SMM_MASK		  (1 << 6)
#define V_INTR_MASK               (1 << 24)

#define IOPM_ALLOC_ORDER          2

#define SEG_TYPE_LDT              2
#define SEG_TYPE_AVAIL_TSS16      3

#define INVALID_PAGE              (~(hpa_t)0)

#define VMRUN_MAX_VCPUS		 288
#define VMRUN_SOFT_MAX_VCPUS	 240
#define VMRUN_MAX_VCPU_ID	 1023

#define VMRUN_USER_MEM_SLOTS	 509
#define VMRUN_PRIVATE_MEM_SLOTS	 3 /* memory slots that are not exposed to userspace */
#define VMRUN_MEM_SLOTS_NUM	 (VMRUN_USER_MEM_SLOTS + VMRUN_PRIVATE_MEM_SLOTS)
#define VMRUN_NR_PAGE_SIZES	 3
#define VMRUN_ADDRESS_SPACE_NUM	 2

#define VMRUN_HPAGE_GFN_SHIFT(x) (((x) - 1) * 9)
#define VMRUN_HPAGE_SHIFT(x)	 (PAGE_SHIFT + VMRUN_HPAGE_GFN_SHIFT(x))
#define VMRUN_HPAGE_SIZE(x)	 (1UL << VMRUN_HPAGE_SHIFT(x))
#define VMRUN_PAGES_PER_HPAGE(x) (VMRUN_HPAGE_SIZE(x) / PAGE_SIZE)

/*
 * The bit 16 ~ bit 31 of vmrun_memory_region::flags are internally used
 * in vmrun, other bits are visible for userspace
 */
#define VMRUN_MEMSLOT_INVALID	(1UL << 16)

#define VMRUN_REQUEST_MASK	GENMASK(7,0)
#define VMRUN_REQUEST_NO_WAKEUP	BIT(8)
#define VMRUN_REQUEST_WAIT	BIT(9)
/*
 * Architecture-independent vcpu->requests bit members
 * Bits 4-7 are reserved for more arch-independent bits.
 */
#define VMRUN_REQ_TLB_FLUSH         (0 | VMRUN_REQUEST_WAIT | VMRUN_REQUEST_NO_WAKEUP)

#define VMRUN_CR0_SELECTIVE_MASK  (X86_CR0_TS | X86_CR0_MP)

#define VMRUN_TSS_PRIVATE_MEMSLOT			(VMRUN_USER_MEM_SLOTS + 0)
#define VMRUN_APIC_ACCESS_PAGE_PRIVATE_MEMSLOT		(VMRUN_USER_MEM_SLOTS + 1)
#define VMRUN_IDENTITY_PAGETABLE_PRIVATE_MEMSLOT	(VMRUN_USER_MEM_SLOTS + 2)

#define PFERR_PRESENT_BIT 0
#define PFERR_WRITE_BIT 1
#define PFERR_USER_BIT 2
#define PFERR_RSVD_BIT 3
#define PFERR_FETCH_BIT 4
#define PFERR_PK_BIT 5
#define PFERR_GUEST_FINAL_BIT 32
#define PFERR_GUEST_PAGE_BIT 33

#define PFERR_PRESENT_MASK (1U << PFERR_PRESENT_BIT)
#define PFERR_WRITE_MASK (1U << PFERR_WRITE_BIT)
#define PFERR_USER_MASK (1U << PFERR_USER_BIT)
#define PFERR_RSVD_MASK (1U << PFERR_RSVD_BIT)
#define PFERR_FETCH_MASK (1U << PFERR_FETCH_BIT)
#define PFERR_PK_MASK (1U << PFERR_PK_BIT)
#define PFERR_GUEST_FINAL_MASK (1ULL << PFERR_GUEST_FINAL_BIT)
#define PFERR_GUEST_PAGE_MASK (1ULL << PFERR_GUEST_PAGE_BIT)

#define SVM_VMMCALL ".byte 0x0f, 0x01, 0xd9"

enum {
	VMCB_INTERCEPTS, /* Intercept vectors, TSC offset,
			    pause filter count */
	VMCB_PERM_MAP,   /* IOPM Base and MSRPM Base */
	VMCB_ASID,	 /* ASID */
	VMCB_INTR,	 /* int_ctl, int_vector */
	VMCB_NPT,        /* npt_en, nCR3, gPAT */
	VMCB_CR,	 /* CR0, CR3, CR4, EFER */
	VMCB_DR,         /* DR6, DR7 */
	VMCB_DT,         /* GDT, IDT */
	VMCB_SEG,        /* CS, DS, SS, ES, CPL */
	VMCB_CR2,        /* CR2 only */
	VMCB_LBR,        /* DBGCTL, BR_FROM, BR_TO, LAST_EX_FROM, LAST_EX_TO */
	VMCB_AVIC,       /* AVIC APIC_BAR, AVIC APIC_BACKING_PAGE,
			  * AVIC PHYSICAL_TABLE pointer,
			  * AVIC LOGICAL_TABLE pointer
			  */
	VMCB_DIRTY_MAX,
};

/* TPR and CR2 are always written before VMRUN */
#define VMCB_ALWAYS_DIRTY_MASK	((1U << VMCB_INTR) | (1U << VMCB_CR2))

enum vmrun_reg {
	VCPU_REGS_RAX = 0,
	VCPU_REGS_RCX = 1,
	VCPU_REGS_RDX = 2,
	VCPU_REGS_RBX = 3,
	VCPU_REGS_RSP = 4,
	VCPU_REGS_RBP = 5,
	VCPU_REGS_RSI = 6,
	VCPU_REGS_RDI = 7,
	VCPU_REGS_R8  = 8,
	VCPU_REGS_R9  = 9,
	VCPU_REGS_R10 = 10,
	VCPU_REGS_R11 = 11,
	VCPU_REGS_R12 = 12,
	VCPU_REGS_R13 = 13,
	VCPU_REGS_R14 = 14,
	VCPU_REGS_R15 = 15,
	VCPU_REGS_RIP,
	NR_VCPU_REGS
};

enum vmrun_reg_ex {
	VCPU_EXREG_PDPTR = NR_VCPU_REGS,
	VCPU_EXREG_CR3,
	VCPU_EXREG_RFLAGS,
	VCPU_EXREG_SEGMENTS,
};

enum {
	VCPU_SREG_ES,
	VCPU_SREG_CS,
	VCPU_SREG_SS,
	VCPU_SREG_DS,
	VCPU_SREG_FS,
	VCPU_SREG_GS,
	VCPU_SREG_TR,
	VCPU_SREG_LDTR,
};

enum {
	OUTSIDE_GUEST_MODE,
	IN_GUEST_MODE,
	EXITING_GUEST_MODE,
	READING_SHADOW_PAGE_TABLES,
};

struct system_table {
	u16 limit;
	u64 base;
} __attribute__ ((__packed__));

struct ldttss_desc {
	u16 limit0;
	u16 base0;
	unsigned base1:8, type:5, dpl:2, p:1;
	unsigned limit1:4, zero0:3, g:1, base2:8;
	u32 base3;
	u32 zero1;
} __attribute__((packed));

struct vmrun_cpu_data {
	int cpu;
	u64 asid_generation;
	u32 max_asid;
	u32 next_asid;
	struct ldttss_desc *tss_desc;
	struct page *save_area;
};

struct vmrun_vcpu;

struct vmrun_mmu {
	void (*new_cr3)(struct vmrun_vcpu *vcpu);
	int (*page_fault)(struct vmrun_vcpu *vcpu, gva_t gva, u32 err);
	void (*inval_page)(struct vmrun_vcpu *vcpu, gva_t gva);
	void (*free)(struct vmrun_vcpu *vcpu);
	gpa_t (*gva_to_gpa)(struct vmrun_vcpu *vcpu, gva_t gva);
	hpa_t root_hpa;
	int root_level;
	int shadow_root_level;

	/*
	 * Bitmap; bit set = permission fault
	 * Byte index: page fault error code [4:1]
	 * Bit index: pte permissions in ACC_* format
	 */
	u8 permissions[16];
};


struct vmrun_vcpu {
	struct vmrun *vmrun;
	struct preempt_notifier preempt_notifier;
	int cpu;
	int vcpu_id;
	int srcu_idx;
	int mode;
	unsigned long requests;
	int pre_pcpu;
	struct list_head blocked_vcpu_list;
	struct mutex mutex;
	struct vmrun_run *run;
	struct pid __rcu *pid;

	/*
	 * [CONFIG_HAVE_KVM_CPU_RELAX_INTERCEPT]
	 * Cpu relax intercept or pause loop exit optimization
	 * in_spin_loop: set when a vcpu does a pause loop exit
	 *  or cpu relax intercepted.
	 * dy_eligible: indicates whether vcpu is eligible for directed yield.
	 */
	struct {
		bool in_spin_loop;
		bool dy_eligible;
	} spin_loop;

	bool preempted;

	struct vmcb *vmcb;
	unsigned long vmcb_pa;
	struct vmrun_cpu_data *cpu_data;
	uint64_t asid_generation;
	uint64_t sysenter_esp;
	uint64_t sysenter_eip;
	u64 next_rip;
	struct {
		u16 fs;
		u16 gs;
		u16 ldt;
		u64 gs_base;
	} host;
	//u32 *msrpm;
	/*
	 * rip and regs accesses must go through
	 * vmrun_{register,rip}_{read,write} functions.
	 */
	unsigned long regs[NR_VCPU_REGS];
	u32 regs_avail;
	u32 regs_dirty;
	unsigned long cr0;
	unsigned long cr0_guest_owned_bits;
	unsigned long cr2;
	unsigned long cr3;
	unsigned long cr4;
	unsigned long cr4_guest_owned_bits;
	unsigned long cr8;
	u32 hflags;
	u64 efer;
	int mp_state;

	struct vmrun_mmu mmu;
	struct list_head free_pages;
};

struct vmrun_rmap_head {
	unsigned long val;
};

struct vmrun_lpage_info {
	int disallow_lpage;
};

struct vmrun_arch_memory_slot {
	struct vmrun_rmap_head *rmap[VMRUN_NR_PAGE_SIZES];
	struct vmrun_lpage_info *lpage_info[VMRUN_NR_PAGE_SIZES - 1];
	unsigned short *gfn_track[VMRUN_PAGE_TRACK_MAX];
};

/*
 * VMRUN_SET_USER_MEMORY_REGION ioctl allows the following operations:
 * - create a new memory slot
 * - delete an existing memory slot
 * - modify an existing memory slot
 *   -- move it in the guest physical memory space
 *   -- just change its flags
 *
 * Since flags can be changed by some of these operations, the following
 * differentiation is the best we can do for __vmrun_set_memory_region():
 */
enum vmrun_mr_change {
	VMRUN_MR_CREATE,
	VMRUN_MR_DELETE,
	VMRUN_MR_MOVE,
	VMRUN_MR_FLAGS_ONLY,
};

/*
 * Some of the bitops functions do not support too long bitmaps.
 * This number must be determined not to exceed such limits.
 */
#define VMRUN_MEM_MAX_NR_PAGES ((1UL << 31) - 1)

struct vmrun_memory_slot {
	gfn_t base_gfn;
	unsigned long npages;
	unsigned long *dirty_bitmap;
	struct vmrun_arch_memory_slot arch;
	unsigned long userspace_addr;
	u32 flags;
	short id;
};

struct vmrun_memslots {
	u64 generation;
	struct vmrun_memory_slot memslots[VMRUN_MEM_SLOTS_NUM];
	/* The mapping table from slot id to the index in memslots[]. */
	short id_to_index[VMRUN_MEM_SLOTS_NUM];
	atomic_t lru_slot;
	int used_slots;
};

struct vmrun {
	spinlock_t mmu_lock;
	struct mutex slots_lock;
	struct mm_struct *mm; /* userspace tied to this vm */
	struct vmrun_memslots __rcu *memslots[VMRUN_ADDRESS_SPACE_NUM];
	struct vmrun_vcpu *vcpus[VMRUN_MAX_VCPUS];

	/*
	 * created_vcpus is protected by vmrun->lock, and is incremented
	 * at the beginning of VMRUN_CREATE_VCPU.  online_vcpus is only
	 * incremented after storing the vmrun_vcpu pointer in vcpus,
	 * and is accessed atomically.
	 */

	atomic_t online_vcpus;
	int created_vcpus;
	int last_boosted_vcpu;
	struct list_head vm_list;
	struct mutex lock;
	atomic_t users_count;

	unsigned int n_used_mmu_pages;
	unsigned int n_requested_mmu_pages;
	unsigned int n_max_mmu_pages;

	struct mmu_notifier mmu_notifier;
	unsigned long mmu_notifier_seq;
	long mmu_notifier_count;

	long tlbs_dirty;
	struct srcu_struct srcu;
	struct list_head active_mmu_pages;
	struct list_head zapped_obsolete_pages;
	struct list_head assigned_dev_head;
	atomic_t noncoherent_dma_count;
	struct hlist_head mask_notifier_list; /* reads protected by irq_srcu, writes by irq_lock */
};

#endif // VMRUN_H
