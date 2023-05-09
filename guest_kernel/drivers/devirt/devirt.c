#include <linux/init.h>
#include <linux/module.h>
#include <linux/hugetlb.h>
#include <asm/tlbflush.h>
#include <linux/kobject.h>
#include <linux/devirt.h>
#include <linux/pagewalk.h>
#include <asm/pgalloc.h>
#include <asm/text-patching.h>
#include <linux/set_memory.h>

#undef pr_fmt
#define pr_fmt(fmt)	"%s.c:%d %s " fmt, KBUILD_MODNAME, __LINE__, __func__

struct devirt_pci_device *dv_dev;
char devirt_task_name[TASK_COMM_LEN] __read_mostly = {0};


int devirt_ptep_set_access_flags(unsigned long address, pte_t *ptep,
			  pte_t entry, int dirty)
{
	int changed;
	pte_t pte, *dptep;

	pte = *ptep;
	changed = !pte_same(pte, entry);

	dptep = __get_dptep(ptep);
	if (likely(dptep))
		changed |= (pte_flags(pte) != pte_flags(*dptep));

	if (changed && dirty)
		devirt_set_pte(ptep, entry);

	return changed;
}

int devirt_pmdp_set_access_flags(unsigned long address, pmd_t *pmdp,
			  pmd_t entry, int dirty)
{
	int changed;
	pmd_t pmd, *dpmdp;

	pmd = *pmdp;
	changed = !pmd_same(pmd, entry);

	dpmdp = __get_dpmdp(pmdp);
	if (likely(dpmdp))
		changed |= (pmd_flags(pmd) != pmd_flags(*dpmdp));

	if (changed && dirty) {
		devirt_set_pmd(pmdp, entry);
		/*
		 * We had a write-protection fault here and changed the pmd
		 * to to more permissive. No need to flush the TLB for that,
		 * #PF is architecturally guaranteed to do that and in the
		 * worst-case we'll generate a spurious fault.
		 */
	}

	return changed;
}

int devirt_pudp_set_access_flags(unsigned long address, pud_t *pudp,
		pud_t entry, int dirty)
{
	int changed;
	pud_t pud, *dpudp;

	pud = *pudp;
	changed = !pud_same(pud, entry);

	dpudp = __get_dpudp(pudp);
	if (likely(dpudp))
		changed |= (pud_flags(pud) != pud_flags(*dpudp));

	VM_BUG_ON(address & ~HPAGE_PUD_MASK);

	if (changed && dirty) {
		devirt_set_pud(pudp, entry);
		/*
		 * We had a write-protection fault here and changed the pud
		 * to to more permissive. No need to flush the TLB for that,
		 * #PF is architecturally guaranteed to do that and in the
		 * worst-case we'll generate a spurious fault.
		 */
	}

	return changed;
}

int devirt_ptep_test_and_clear_young(struct vm_area_struct *vma,
			      unsigned long addr, pte_t *ptep)
{
	int ret = 0;
	pte_t *dptep;

	if (pte_young(*ptep))
		ret = test_and_clear_bit(_PAGE_BIT_ACCESSED,
					 (unsigned long *) &ptep->pte);

	dptep = __get_dptep(ptep);

	if (unlikely(!dptep))
		return ret;

	if (pte_young(*dptep))
		ret |= test_and_clear_bit(_PAGE_BIT_ACCESSED,
					 (unsigned long *) &dptep->pte);

	return ret;
}

int devirt_pmdp_test_and_clear_young(struct vm_area_struct *vma,
			      unsigned long addr, pmd_t *pmdp)
{
	int ret = 0;
	pmd_t *dpmdp;

	if (pmd_young(*pmdp))
		ret = test_and_clear_bit(_PAGE_BIT_ACCESSED,
					 (unsigned long *)pmdp);

	dpmdp = __get_dpmdp(pmdp);

	if (unlikely(!dpmdp))
		return ret;

	if (pmd_young(*dpmdp))
		ret |= test_and_clear_bit(_PAGE_BIT_ACCESSED,
					 (unsigned long *)dpmdp);

	return ret;
}

int devirt_pudp_test_and_clear_young(struct vm_area_struct *vma,
			      unsigned long addr, pud_t *pudp)
{
	int ret = 0;
	pud_t *dpudp;

	if (pud_young(*pudp))
		ret = test_and_clear_bit(_PAGE_BIT_ACCESSED,
					 (unsigned long *)pudp);

	dpudp = __get_dpudp(pudp);

	if (unlikely(!dpudp))
		return ret;

	if (pud_young(*dpudp))
		ret |= test_and_clear_bit(_PAGE_BIT_ACCESSED,
					 (unsigned long *)dpudp);

	return ret;
}


static ssize_t task_show(struct kobject *kobj, struct kobj_attribute *attr,
		char *buf)
{
	return sprintf(buf, "%s\n", devirt_task_name);
}

static ssize_t task_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t count)
{
	ssize_t ret = count;

	if (unlikely(!dv_dev->dv_root) && devirt_init_dv_root())
		return 0;

	strcpy(devirt_task_name, buf);
	return ret;
}

static struct kobj_attribute task_name_attr =
	__ATTR(task_name, 0644, task_show, task_store);

static struct attribute *devirt_attr[] = {
	&task_name_attr.attr,
	NULL,
};

static const struct attribute_group devirt_attr_group = {
	.attrs = devirt_attr,
	.name = "devirt",
};

static int devirt_init_sysfs(void)
{
	int err;

	err = sysfs_create_group(mm_kobj, &devirt_attr_group);
	if (err) {
		pr_err("devirt: failed to register devirt sysfs group %d\n",
				err);
		return err;
	}

	return 0;
}

static void walk_all_ranges(struct mm_struct *mm, pgd_t *pgd,
		const struct mm_walk_ops *ops, void *p)
{
	walk_page_range_novma(mm, 0UL, PTRS_PER_PGD * PGD_LEVEL_MULT / 2,
			ops, pgd, p);
	walk_page_range_novma(mm, GUARD_HOLE_END_ADDR, ~0UL, ops, pgd, p);
}

struct dvroot_state {
	int level;
	unsigned int entries_changed;
	unsigned int cur_page;
	p4d_t *cur_p4d_page;
};


static int dvroot_p4d_entry(p4d_t *p4d, unsigned long addr,
			    unsigned long next, struct mm_walk *walk)
{
	struct dvroot_state *ds = walk->private;
	unsigned long cur_pud_page;
	/* no leaf needed here - no such huge page */

	if (ds->level == -1) {
		ds->level = 0;
		ds->cur_p4d_page = (p4d_t *)dv_dev->dv_root_pages[ds->cur_page];
		ds->cur_page++;
	}

	ds->entries_changed++;
	ds->level = 0;

	cur_pud_page = p4d_page(*p4d)->_pt_pad_2;
	if (!cur_pud_page) {
		cur_pud_page = dv_dev->dv_root_pages[ds->cur_page];
		p4d_page(*p4d)->_pt_pad_2 = cur_pud_page;
		ds->cur_page++;
	}

	devirt_copy_p4d_val((ds->cur_p4d_page + PGT_OFF(p4d)), p4d,
		cur_pud_page);

	return 0;
}

static int dvroot_pud_entry(pud_t *pud, unsigned long addr,
			    unsigned long next, struct mm_walk *walk)
{
	struct dvroot_state *ds = walk->private;
	unsigned long cur_pmd_page;
	pud_t *cur_pud_page;

	cur_pud_page = (pud_t *)__get_devirt_page((unsigned long)pud);
	if (!cur_pud_page) {
		pr_info("Could not find pud page\n");
		BUG();
	}

	if (pud_huge(*pud)) {
		ds->entries_changed++;
		devirt_copy_pud(cur_pud_page + PGT_OFF(pud), pud);
		pr_info("HUGE PUD\n");
		BUG();
		return 0;
	}

	ds->entries_changed++;
	ds->level = 1;

	cur_pmd_page = pud_page(*pud)->_pt_pad_2;
	if (!cur_pmd_page) {
		cur_pmd_page = dv_dev->dv_root_pages[ds->cur_page];
		pud_page(*pud)->_pt_pad_2 = cur_pmd_page;
		ds->cur_page++;
	}

	devirt_copy_pud_val((cur_pud_page + PGT_OFF(pud)), pud, cur_pmd_page);

	return 0;
}

static int dvroot_pmd_entry(pmd_t *pmd, unsigned long addr,
			    unsigned long next, struct mm_walk *walk)
{
	struct dvroot_state *ds = walk->private;
	unsigned long cur_pte_page, cur_dst;
	pmd_t *cur_pmd_page;

	cur_pmd_page = (pmd_t *)__get_devirt_page((unsigned long)pmd);
	if (!cur_pmd_page) {
		pr_info("Could not find pmd page\n");
		BUG();
	}

	if (pmd_huge(*pmd)) {
		int i;
		pte_t *ptep;
		ds->entries_changed++;
		cur_pte_page = dv_dev->dv_root_pages[ds->cur_page];
		ptep = (pte_t *)cur_pte_page;
		ds->cur_page++;
		WRITE_ONCE((cur_pmd_page + PGT_OFF(pmd))->pmd,
				PGT_VAL_NO_HUGE(pmd_val(*pmd), cur_pte_page));

		cur_dst = (unsigned long)pfn_to_kaddr(pmd_pfn(*pmd));
		for (i = 0; i < PTRS_PER_PTE; i++, ptep++, cur_dst += PAGE_SIZE) {
			check_devirt_present((unsigned long)pte_val(*ptep), cur_dst);
			WRITE_ONCE(ptep->pte, PGT_VAL_NO_HUGE(pmd_val(*pmd), cur_dst));
		}

		return 0;
	}

	ds->entries_changed++;
	ds->level = 2;

	cur_pte_page = pmd_page(*pmd)->_pt_pad_2;
	if (!cur_pte_page) {
		cur_pte_page = dv_dev->dv_root_pages[ds->cur_page];
		pmd_page(*pmd)->_pt_pad_2 = cur_pte_page;
		ds->cur_page++;
	}

	devirt_copy_pmd_val((cur_pmd_page + PGT_OFF(pmd)), pmd, cur_pte_page);
	return 0;
}

static int dvroot_pte_entry(pte_t *pte, unsigned long addr,
			    unsigned long next, struct mm_walk *walk)
{
	struct dvroot_state *ds = walk->private;
	pte_t *cur_pte_page;

	cur_pte_page = (pte_t *)__get_devirt_page((unsigned long)pte);
	if (!cur_pte_page) {
		pr_info("Could not find pte page\n");
		BUG();
	}

	ds->entries_changed++;
	ds->level = 3;
	devirt_copy_pte((cur_pte_page + PGT_OFF(pte)), pte);
	return 0;
}

static const struct mm_walk_ops dvroot_ops = {
	.p4d_entry	= dvroot_p4d_entry,
	.pud_entry	= dvroot_pud_entry,
	.pmd_entry	= dvroot_pmd_entry,
	.pte_entry	= dvroot_pte_entry,
};

struct ptcount_state {
	unsigned int pt_count[3];
};

static int count_p4d_entry(p4d_t *p4d, unsigned long addr,
			    unsigned long next, struct mm_walk *walk)
{
	struct ptcount_state *ps = walk->private;
	if (!p4d_huge(*p4d))
		ps->pt_count[0]++;
	return 0;
}

static int count_pud_entry(pud_t *pud, unsigned long addr,
			    unsigned long next, struct mm_walk *walk)
{
	struct ptcount_state *ps = walk->private;
	if (!pud_huge(*pud))
		ps->pt_count[1]++;
	return 0;
}

static int count_pmd_entry(pmd_t *pmd, unsigned long addr,
			    unsigned long next, struct mm_walk *walk)
{
	struct ptcount_state *ps = walk->private;
	/* We need to break up huge pages... */
	ps->pt_count[2]++;
	return 0;
}

static const struct mm_walk_ops ptcount_ops = {
	.p4d_entry	= count_p4d_entry,
	.pud_entry	= count_pud_entry,
	.pmd_entry	= count_pmd_entry,
};

/* construct dvroot */
static pgd_t *construct_dv_root(struct mm_struct *mm, pgd_t *pgd)
{
	struct dvroot_state ds = {0};

	ds.level = -1;

	mmap_read_lock(mm);
	walk_all_ranges(mm, pgd, &dvroot_ops, &ds);
	mmap_read_unlock(mm);

	return (pgd_t *)ds.cur_p4d_page;
}

/* count the overall number of page tables */
static unsigned int count_pts(struct mm_struct *mm, pgd_t *pgd)
{
	struct ptcount_state ps = {0};

	mmap_read_lock(mm);
	walk_all_ranges(mm, pgd, &ptcount_ops, &ps);
	mmap_read_unlock(mm);

	return (ps.pt_count[0] + ps.pt_count[1] + ps.pt_count[2] + 1);
}

void devirt_touch_pages(void)
{
	struct zone *zone;
	unsigned long n = 0;

	for_each_populated_zone(zone) {
		unsigned long pfn, max_zone_pfn;

		max_zone_pfn = zone_end_pfn(zone);
		for (pfn = zone->zone_start_pfn; pfn < max_zone_pfn; pfn++) {
			struct page *p = pfn_to_page(pfn);

			if (!pfn_valid(pfn) || !p)
				continue;
			if (PageReserved(p) && !kernel_page_present(p))
				continue;

			asm volatile ("" : : "r" (*(unsigned char *)(pfn_to_kaddr(pfn))));
			if (dv_dev)
				_check_devirt_present((unsigned long)pfn_to_kaddr(pfn));
			n++;
			cond_resched();
		}
	}
}

int devirt_init_dv_root(void)
{
	unsigned int i, j;
	unsigned long alloc_size;

	dv_dev->dv_root_num_pages = count_pts(&init_mm,
			kernel_to_user_pgdp(swapper_pg_dir));

	alloc_size = dv_dev->dv_root_num_pages * sizeof(unsigned long);
	dv_dev->dv_root_pages = kmalloc(alloc_size, GFP_KERNEL);
	if (!dv_dev->dv_root_pages) {
		pr_err("could not allocate pages array\n");
		goto err_alloc_array;
	}

	for (i = 0; i < dv_dev->dv_root_num_pages; i++) {
		dv_dev->dv_root_pages[i] = __get_free_page(GFP_PGTABLE_DEVIRT);
		if (!dv_dev->dv_root_pages[i]) {
			pr_err("could not allocate pt pages\n");
			goto err_alloc_page;
		}
	}

	dv_dev->dv_root = construct_dv_root(&init_mm,
			kernel_to_user_pgdp(swapper_pg_dir));

	return 0;

err_alloc_page:
	for (j = 0; j < i; j++)
		free_page(dv_dev->dv_root_pages[j]);

	kfree(dv_dev->dv_root_pages);
err_alloc_array:
	return -1;
}

static void devirt_remove_dv_root(void)
{
	int i;

	for (i = 0; i < dv_dev->dv_root_num_pages; i++)
		free_page(dv_dev->dv_root_pages[i]);

	kfree(dv_dev->dv_root_pages);
	dv_dev->dv_root = NULL;
}

static void devirt_pci_remove(struct pci_dev *pdev)
{
	if (likely(dv_dev->dv_root))
		devirt_remove_dv_root();

	pci_iounmap(pdev, dv_dev->gfn_to_hfn);
	pci_iounmap(pdev, dv_dev->ioaddr);
	pci_release_region(pdev, 2);
	pci_release_region(pdev, 0);
	pci_disable_device(pdev);
	kfree(dv_dev);
}

static int devirt_pci_probe(struct pci_dev *pdev,
		const struct pci_device_id *id)
{
	int ret = -EINVAL;

	dv_dev = kzalloc(sizeof(struct devirt_pci_device), GFP_KERNEL);
	if (!dv_dev)
		return -ENOMEM;

	pci_set_drvdata(pdev, dv_dev);
	dv_dev->pdev = pdev;

	ret = pci_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "could not enable device %d\n", ret);
		goto err_enable_device;
	}

	/* iomap register BAR */
	ret = pci_request_region(pdev, 0, "devirt-pci");
	if (ret) {
		dev_err(&pdev->dev, "could not request BAR 0 %d\n", ret);
		goto err_request_region;
	}

	ret = pci_request_region(pdev, 2, "devirt-pci");
	if (ret) {
		dev_err(&pdev->dev, "could not request BAR 2 %d\n", ret);
		goto err_request_region2;
	}

	dv_dev->ioaddr = pci_iomap(pdev, 0, 0);
	if (!dv_dev->ioaddr) {
		dev_err(&pdev->dev, "could not map BAR 0 %d\n", ret);
		goto err_request_map;
	}

	pci_set_master(pdev);

	dv_dev->gfn_to_hfn_len = ioread32(dv_dev->ioaddr + 16);
	if (!dv_dev->gfn_to_hfn_len) {
		dev_err(&pdev->dev, "could not get map len\n");
		goto err_request_len;
	}

	dv_dev->gfn_to_hfn = pci_iomap_wb_range(pdev, 2, 0,
			dv_dev->gfn_to_hfn_len);
	if (!dv_dev->gfn_to_hfn) {
		dev_err(&pdev->dev, "could not map BAR 0 %d\n", ret);
		goto err_request_map_gfn;
	}

	if (ret) {
		dev_err(&pdev->dev, "could not initalize dv_root\n");
		goto err_dv_root;
	}

	if (devirt_init_sysfs()) {
		dev_err(&pdev->dev, "could not initialize sysfs\n");
		goto err_init_sysfs;
	}
	kvm_hypercall0(KVM_HC_INIT_DEVIRT);

	return 0;

err_init_sysfs:
err_dv_root:
	pci_iounmap(pdev, dv_dev->gfn_to_hfn);
err_request_map_gfn:
err_request_len:
	pci_iounmap(pdev, dv_dev->ioaddr);
err_request_map:
err_request_region2:
	pci_release_region(pdev, 0);
err_request_region:
	pci_disable_device(pdev);
err_enable_device:
	kfree(dv_dev);
	dv_dev = NULL;
	return ret;
}

static const struct pci_device_id devirt_pci_id_table[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_DEVIRT_CLASS, PCI_ANY_ID) },
	{ 0 }
};

static struct pci_driver devirt_pci_driver = {
	.name		= "devirt-pci",
	.id_table	= devirt_pci_id_table,
	.probe		= devirt_pci_probe,
	.remove		= devirt_pci_remove,
};

MODULE_DEVICE_TABLE(pci, devirt_pci_id_table);

module_pci_driver(devirt_pci_driver);

MODULE_AUTHOR("Shai Bergman <shai@shai.pub>");
MODULE_DESCRIPTION("devirt");
MODULE_LICENSE("GPL");
MODULE_VERSION("1");
