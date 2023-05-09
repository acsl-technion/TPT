#ifndef __DEVIRT_H__
#define __DEVIRT_H__

#include <linux/types.h>
#include <linux/binfmts.h>
#include <linux/sched/task.h>
#include <linux/kvm_para.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/hugetlb.h>
#include <linux/pgtable.h>

#define PCI_VENDOR_ID_DEVIRT_CLASS 0x10ef
#define GFP_PGTABLE_DEVIRT	(GFP_ATOMIC | __GFP_ZERO)
#define DEVIRT "devirt_debug: "
#define virt_to_pfn(v)          (PFN_DOWN(__pa(v)))

#define PTE_LEVEL_MULT (PAGE_SIZE)
#define PMD_LEVEL_MULT (PTRS_PER_PTE * PTE_LEVEL_MULT)
#define PUD_LEVEL_MULT (PTRS_PER_PMD * PMD_LEVEL_MULT)
#define P4D_LEVEL_MULT (PTRS_PER_PUD * PUD_LEVEL_MULT)
#define PGD_LEVEL_MULT (PTRS_PER_P4D * P4D_LEVEL_MULT)
#define virt_to_pfn(v)          (PFN_DOWN(__pa(v)))

#define PGT_OFF(off)		(((unsigned long)off >> (3U)) & 511U)


#define GFN_VAL(val)			(dv_dev->gfn_to_hfn[virt_to_pfn(val)] << PAGE_SHIFT)

#define PGT_VAL_NO_HUGE(src, val)	((src & _PAGE_PRESENT) ? \
					((src & (PTE_FLAGS_MASK & ~_PAGE_PSE)) | \
					GFN_VAL(val)) : 0UL)

#define PGT_VAL(src, val)	((src & _PAGE_PRESENT) ? \
				((src & PTE_FLAGS_MASK) | GFN_VAL(val)) : 0UL)

struct devirt_pci_device {
	struct pci_dev *pdev;
	void __iomem *ioaddr;
	u32 gfn_to_hfn_len;
	u64 *gfn_to_hfn;
	pgd_t *dv_root;
	unsigned long *dv_root_pages;
	u32 dv_root_num_pages;
};

extern struct devirt_pci_device *dv_dev;
extern char devirt_task_name[];
int devirt_init_dv_root(void);

static inline void _check_devirt_present(unsigned long val) {

	if (unlikely(!pfn_valid(virt_to_pfn(val))))
		return;

	asm volatile ("" : : "r" (*(unsigned char *)(val)));
}

static inline void check_devirt_present(unsigned long src, unsigned long val)
{
	if(!(src & _PAGE_PRESENT))
		return;

	if(likely(dv_dev->gfn_to_hfn[virt_to_pfn(val)]))
		return;

	_check_devirt_present(val);
}

static inline void devirt_copy_p4d_val(p4d_t *dst, p4d_t *src,
		unsigned long val)
{
	check_devirt_present((unsigned long)p4d_val(*src), val);
	WRITE_ONCE(dst->pgd.pgd, PGT_VAL(p4d_val(*src), val));
}

static inline void devirt_copy_pud_val(pud_t *dst, pud_t *src,
		unsigned long val)
{
	check_devirt_present((unsigned long)pud_val(*src), val);
	WRITE_ONCE(dst->pud, PGT_VAL(pud_val(*src), val));
}

static inline void devirt_copy_pud(pud_t *dst, pud_t *src)
{
	devirt_copy_pud_val(dst, src,
			(unsigned long)pfn_to_kaddr(pud_pfn((*src))));
}

static inline void devirt_copy_pmd_val(pmd_t *dst, pmd_t *src,
		unsigned long val)
{
	check_devirt_present((unsigned long)pmd_val(*src), val);
	WRITE_ONCE(dst->pmd, PGT_VAL(pmd_val(*src), val));
}

static inline void devirt_copy_pmd(pmd_t *dst, pmd_t *src)
{
	devirt_copy_pmd_val(dst, src,
			(unsigned long)pfn_to_kaddr(pmd_pfn((*src))));
}

static inline void devirt_copy_pte_val(pte_t *dst, pte_t *src,
		unsigned long val)
{
	check_devirt_present((unsigned long)pte_val(*src), val);
	WRITE_ONCE(dst->pte, PGT_VAL(pte_val(*src), val));
}

static inline void devirt_copy_pte(pte_t *dst, pte_t *src)
{
	devirt_copy_pte_val(dst, src,
			(unsigned long)pfn_to_kaddr(pte_pfn(*src)));
}

static inline void _devirt_alloc_pt(struct mm_struct *mm, unsigned long pfn)
{
	unsigned long dpfn;

	if (READ_ONCE(pfn_to_page(pfn)->_pt_pad_2))
		return;

	dpfn = __get_free_page(GFP_PGTABLE_DEVIRT);
	if (!dpfn) {
		pr_err("could not allocate new pt page\n");
		BUG();
	}

	WRITE_ONCE(pfn_to_page(pfn)->_pt_pad_2, dpfn);
}

static inline int devirt_pgd_alloc(struct mm_struct *mm)
{
	unsigned long pfn;

	if (unlikely(mm == &init_mm))
		return 0;

	if (!test_bit(MMF_DEVIRT, &mm->flags))
		return 0;

	pfn = virt_to_pfn((unsigned long)mm->pgd);

	memcpy((pgd_t *)pfn_to_kaddr(pfn + 3) + KERNEL_PGD_BOUNDARY,
		(dv_dev->dv_root + KERNEL_PGD_BOUNDARY),
		KERNEL_PGD_PTRS * sizeof(pgd_t));

	return 0;
}

static inline void initialize_devirt_proc(struct linux_binprm *bprm)
{
	const char *task_name = kbasename(bprm->filename);

	if (unlikely(bprm->mm == &init_mm))
		return;

	if(!strlen(devirt_task_name) || !strlen(task_name))
		return;

	if (!sysfs_streq(devirt_task_name, task_name))
		return;

	set_bit(MMF_DEVIRT, &bprm->mm->flags);

	if (devirt_pgd_alloc(bprm->mm)) {
		pr_err("could not allocate pgd\n");
		BUG();
	}

}

static inline void devirt_release_pt(unsigned long pfn)
{
	if (!pfn_to_page(pfn)->_pt_pad_2)
		return;

	free_page(pfn_to_page(pfn)->_pt_pad_2);
	WRITE_ONCE(pfn_to_page(pfn)->_pt_pad_2, 0UL);
}


static inline void devirt_alloc_pt(struct mm_struct *mm, unsigned long pfn)
{

	if (!test_bit(MMF_DEVIRT, &mm->flags))
		return;

	if (unlikely(!mm || mm == &init_mm))
		return;
	_devirt_alloc_pt(mm, pfn);
}

static inline void devirt_alloc_pte_one(struct mm_struct *mm, unsigned long pfn)
{
	devirt_alloc_pt(mm, pfn);
}

static inline void devirt_alloc_pmd_one(struct mm_struct *mm, unsigned long pfn)
{
	devirt_alloc_pt(mm, pfn);
}

static inline void devirt_alloc_pud_one(struct mm_struct *mm, unsigned long pfn)
{
	devirt_alloc_pt(mm, pfn);
}

static inline void devirt_release_pte_one(unsigned long pfn)
{
	devirt_release_pt(pfn);
}

static inline void devirt_release_pmd_one(unsigned long pfn)
{
	devirt_release_pt(pfn);
}

static inline void devirt_release_pud_one(unsigned long pfn)
{
	devirt_release_pt(pfn);
}

static inline unsigned long __get_devirt_page(unsigned long addr)
{
	struct page *pt_page;
	unsigned long pfn;

	pfn = virt_to_pfn((unsigned long)addr);
	pt_page = pfn_to_page(pfn);

	return pt_page->_pt_pad_2;
}

static inline pud_t __join_pud_flags(pud_t pud, pud_t dpud)
{
	pud.pud |= pud_flags(dpud);
	return pud;
}

static inline pmd_t __join_pmd_flags(pmd_t pmd, pmd_t dpmd)
{
	return pmd;
}

static inline pte_t __join_pte_flags(pte_t pte, pte_t dpte)
{
	pte.pte |= pte_flags(dpte);
	return pte;
}

static inline pud_t *__get_dpudp(pud_t *pudp)
{
	pud_t *dpudp;

	dpudp = (pud_t *)__get_devirt_page((unsigned long)pudp);
	if (unlikely(!dpudp))
		return NULL;

	return (dpudp + PGT_OFF(pudp));
}

static inline pmd_t *__get_dpmdp(pmd_t *pmdp)
{
	pmd_t *dpmdp;

	dpmdp = (pmd_t *)__get_devirt_page((unsigned long)pmdp);
	if (unlikely(!dpmdp))
		return NULL;

	return (dpmdp + PGT_OFF(pmdp));
}

static inline pte_t *__get_dptep(pte_t *ptep)
{
	pte_t *dptep;

	dptep = (pte_t *)__get_devirt_page((unsigned long)ptep);
	if (unlikely(!dptep))
		return NULL;

	return (dptep + PGT_OFF(ptep));
}

static inline void devirt_set_pte(pte_t *ptep, pte_t pte)
{
	pte_t *dptep;

	native_set_pte(ptep, pte);

	dptep = __get_dptep(ptep);
	if (unlikely(!dptep))
		return;

	devirt_copy_pte_val(dptep, ptep, (unsigned long)pfn_to_kaddr(pte_pfn(pte)));
}

static inline void devirt_set_pmd(pmd_t *pmdp, pmd_t pmd)
{
	pmd_t *dpmdp;
	unsigned long pfn;

	native_set_pmd(pmdp, pmd);

	if (!pmdp)
		return;

	pfn = virt_to_pfn((unsigned long)pmdp);
	if (!pfn_valid(pfn) || !PageTable(pfn_to_page(pfn)))
		return;

	dpmdp = __get_dpmdp(pmdp);
	if (unlikely(!dpmdp))
		return;

	if (unlikely(pmd_huge(pmd)))
		devirt_copy_pmd_val(dpmdp, pmdp, (unsigned long)pfn_to_kaddr(pmd_pfn(pmd)));
	else
		devirt_copy_pmd_val(dpmdp, pmdp, (unsigned long)pmd_page(pmd)->_pt_pad_2);
}

static inline void devirt_set_pud(pud_t *pudp, pud_t pud)
{
	pud_t *dpudp;

	native_set_pud(pudp, pud);

	dpudp = __get_dpudp(pudp);
	if (unlikely(!dpudp))
		return;

	if (unlikely(pud_huge(pud)))
		devirt_copy_pud_val(dpudp, pudp, (unsigned long)native_pud_val(pud));
	else
		devirt_copy_pud_val(dpudp, pudp, (unsigned long)pud_page(pud)->_pt_pad_2);
}

static inline void devirt_set_p4d(p4d_t *p4dp, p4d_t p4d)
{
	unsigned long pfn;
	p4d_t *dp4dp;
	struct mm_struct *mm;

	native_set_p4d(p4dp, p4d);

	pfn = virt_to_pfn((unsigned long)p4dp);

	mm = pgd_page_get_mm(pfn_to_page(pfn));
	if (unlikely(!mm || mm == &init_mm || !test_bit(MMF_DEVIRT, &mm->flags)))
		return;

	dp4dp = (p4d_t *)pfn_to_kaddr(pfn + 3) + PGT_OFF(p4dp);
	devirt_copy_p4d_val(dp4dp, p4dp + 512, (unsigned long)p4d_page(p4d)->_pt_pad_2);
}

static inline void devirt_ptep_set_wrprotect(struct mm_struct *mm,
				      unsigned long addr, pte_t *ptep)
{
	pte_t *dptep;

	clear_bit(_PAGE_BIT_RW, (unsigned long *)&ptep->pte);

	dptep = __get_dptep(ptep);
	if (likely(dptep))
		clear_bit(_PAGE_BIT_RW, (unsigned long *)&dptep->pte);
}

extern int devirt_ptep_set_access_flags(unsigned long address, pte_t *ptep,
				 pte_t entry, int dirty);

extern int devirt_ptep_test_and_clear_young(struct vm_area_struct *vma,
				     unsigned long addr, pte_t *ptep);
extern void devirt_touch_pages(void);

static inline pte_t devirt_ptep_get_and_clear(struct mm_struct *mm, unsigned long addr,
				       pte_t *ptep)
{
	pte_t pte, dpte, *dptep;

	pte = native_ptep_get_and_clear(ptep);

	dptep = __get_dptep(ptep);
	if (unlikely(!dptep))
		return pte;

	dpte = native_ptep_get_and_clear(dptep);

	return __join_pte_flags(pte, dpte);
}

extern int devirt_pmdp_set_access_flags(unsigned long address, pmd_t *pmdp,
				 pmd_t entry, int dirty);

extern int devirt_pudp_set_access_flags(unsigned long address, pud_t *pudp,
				 pud_t entry, int dirty);

extern int devirt_pmdp_test_and_clear_young(struct vm_area_struct *vma,
				     unsigned long addr, pmd_t *pmdp);

extern int devirt_pudp_test_and_clear_young(struct vm_area_struct *vma,
				     unsigned long addr, pud_t *pudp);

static inline pmd_t devirt_pmdp_huge_get_and_clear(struct mm_struct *mm, unsigned long addr,
				       pmd_t *pmdp)
{
	pmd_t pmd, dpmd, *dptep;

	pmd = native_pmdp_get_and_clear(pmdp);

	dptep = __get_dpmdp(pmdp);
	if (unlikely(!dptep))
		return pmd;

	dpmd = native_pmdp_get_and_clear(dptep);

	return __join_pmd_flags(pmd, dpmd);
}

static inline pud_t devirt_pudp_huge_get_and_clear(struct mm_struct *mm,
					unsigned long addr, pud_t *pudp)
{
	pud_t pud, dpud, *dpudp;

	pud = native_pudp_get_and_clear(pudp);

	dpudp = __get_dpudp(pudp);
	if (unlikely(!dpudp))
		return pud;

	dpud = native_pudp_get_and_clear(dpudp);

	return __join_pud_flags(pud, dpud);
}

static inline void devirt_pmdp_set_wrprotect(struct mm_struct *mm,
				      unsigned long addr, pmd_t *pmdp)
{
	pmd_t *dpmdp;

	dpmdp = __get_dpmdp(pmdp);
	clear_bit(_PAGE_BIT_RW, (unsigned long *)pmdp);

	if (likely(dpmdp))
		clear_bit(_PAGE_BIT_RW, (unsigned long *)dpmdp);
}

static inline pmd_t devirt_pmdp_establish(struct vm_area_struct *vma,
		unsigned long address, pmd_t *pmdp, pmd_t pmd)
{
	pmd_t dpmd, *dpmdp;

	if (IS_ENABLED(CONFIG_SMP)) {
		pmd_t ret_pmd;
		ret_pmd = xchg(pmdp, pmd);

		dpmdp = __get_dpmdp(pmdp);

		if (unlikely(!dpmdp))
			return ret_pmd;

		if (unlikely(pmd_huge(pmd)))
			dpmd.pmd = (unsigned long)pfn_to_kaddr(pmd_pfn(pmd));
		else
			dpmd.pmd = (unsigned long)pmd_page(pmd)->_pt_pad_2;

		check_devirt_present((unsigned long)pmd_val(pmd), dpmd.pmd);
		dpmd.pmd = PGT_VAL(pmd_val(pmd), dpmd.pmd);
		dpmd = xchg(dpmdp, dpmd);

		return __join_pmd_flags(pmd, dpmd);
	} else {
		pmd_t old = *pmdp;
		WRITE_ONCE(*pmdp, pmd);

		dpmdp = __get_dpmdp(pmdp);

		if (unlikely(!dpmdp))
			return old;

		if (unlikely(pmd_huge(old)))
			dpmd.pmd = (unsigned long)pfn_to_kaddr(pmd_pfn(old));
		else
			dpmd.pmd = (unsigned long)pmd_page(old)->_pt_pad_2;

		WRITE_ONCE(*dpmdp, dpmd);
		return __join_pmd_flags(old, dpmd);
	}
}

#endif
