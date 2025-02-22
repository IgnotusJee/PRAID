#include <linux/pci.h>
#include <linux/irq.h>
#include <linux/version.h>
#include <linux/bio.h>

#include <linux/percpu-defs.h>
#include <linux/sched/clock.h>

#include "device.h"
#include "pci.h"
#include "pciev.h"
#include "praid.h"

static void __signal_irq(const char *type, unsigned int irq)
{
	struct irq_data *data = irq_get_irq_data(irq);
	struct irq_chip *chip = irq_data_get_irq_chip(data);

	PCIEV_DEBUG_VERBOSE("irq: %s %d, vector %d\n", type, irq, irqd_cfg(data)->vector);
	BUG_ON(!chip->irq_retrigger);
	chip->irq_retrigger(data);

	return;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 17, 0)
static void __process_msi_irq(int msi_index)
{
	unsigned int virq = msi_get_virq(&pciev_vdev->pdev->dev, msi_index);

	BUG_ON(virq == 0);
	__signal_irq("msi", virq);
}
#else
static void __process_msi_irq(int msi_index)
{
	struct msi_desc *msi_desc, *tmp;

	for_each_msi_entry_safe(msi_desc, tmp, (&pciev_vdev->pdev->dev)) {
		if (msi_desc->msi_attrib.entry_nr == msi_index) {
			__signal_irq("msi", msi_desc->irq);
			return;
		}
	}
	PCIEV_INFO("Failed to send IPI\n");
	BUG_ON(!msi_desc);
}
#endif

void pciev_signal_irq(int msi_index)
{
	if (pciev_vdev->pdev->msix_enabled) {
		__process_msi_irq(msi_index);
	} else {
		pciev_vdev->pcihdr->sts.is = 1;

		__signal_irq("int", pciev_vdev->pdev->irq);
	}
}

/*
 * The host device driver can change multiple locations in the BAR.
 * In a real device, these changes are processed one after the other,
 * preserving their requesting order. However, in NVMeVirt, the changes
 * can be DETECTED with the dispatcher, obsecuring the order between
 * changes that are made between the checking loop. Thus, we have to
 * process the changes strategically, in an order that are supposed
 * to be...
 *
 * Also, memory barrier is not necessary here since BAR-related
 * operations are only processed by the dispatcher.
 */
void pciev_proc_bars(void)
{
	volatile struct pciev_bar *old_bar = pciev_vdev->old_bar;
	volatile struct pciev_bar *bar = pciev_vdev->bar;

	if (old_bar->dev_cnt != bar->dev_cnt) {
		// memcpy(&old_bar->dev_cnt, &bar->dev_cnt, sizeof(old_bar->dev_cnt));
		bar->dev_cnt = old_bar->dev_cnt; // read only
	}

// out:
	smp_mb();
	return;
}

static enum pciev_io_t {
	PCIEV_BIO_READ = 0,
	PCIEV_BIO_WRITE = 1,
};

static int pciev_submit_bio(void* buffer, size_t offset, size_t size, sector_t sector_num, struct block_device* blk_dev, enum pciev_io_t rw) {
	struct bio *bio;
	void *page_data;
	struct page* page;
	int ret = 0;

	bio = bio_alloc(GFP_KERNEL, 1);
    if(!bio) {
		PCIEV_ERROR("Failed to allocate bio\n");
		ret = -EINVAL;
		goto out;
    }

	page = alloc_page(GFP_KERNEL);
	if(!page) {
		PCIEV_ERROR("Failed to allocate page\n");
		ret = -EINVAL;
		goto out_bio;
	}

	page_data = kmap(page);
	if(!page_data) {
		PCIEV_ERROR("Page map error\n");
		ret = -EINVAL;
		goto out_page;
	}

	if(rw == PCIEV_BIO_WRITE) {
		memcpy((uint8_t*)page_data + offset, (uint8_t*)buffer + offset, size);
	}

	bio_set_dev(bio, blk_dev);
	bio->bi_iter.bi_sector = sector_num;

	if(bio_add_page(bio, page, size, offset) != size) {
		PCIEV_ERROR("Failed to add bio page\n");
		ret = -EIO;
		goto out_map;
	}

	PCIEV_INFO("sta_sector=%llu, size=%lu, offset=%lu", sector_num, size, offset);

	bio_set_op_attrs(bio, rw ? REQ_OP_WRITE : REQ_OP_READ, 0);
	if(submit_bio_wait(bio) < 0) {
		PCIEV_ERROR("Failed to submit bio\n");
		ret = -EIO;
		goto out_map;
	}

	if(rw == PCIEV_BIO_READ) {
		memcpy((uint8_t*)buffer + offset, (uint8_t*)page_data + offset, size);
	}

out_map:
	kunmap(page);
out_page:
	__free_page(page);
out_bio:
	bio_put(bio);
out:
	return ret;
}

void pciev_dispatcher_clac_xor_single(void) {
	uint64_t value, toffset, tsize, nowofs, offset;
	uint8_t *data, *res;

	if(pciev_vdev->bar->io_property.io_num <= pciev_vdev->bar->io_property.io_done) {
		return;
	}

	PCIEV_DEBUG("4\n");

	pciev_vdev->bar->io_property.io_done ++;

	toffset = pciev_vdev->bar->io_property.offset;
	tsize = pciev_vdev->bar->io_property.size;

	data = pciev_vdev->storage_mapped;
	res = data + PAGE_SIZE * 2;

	if(pciev_submit_bio(res, toffset, tsize, pciev_vdev->bar->io_property.sector_sta, pciev_vdev->verify_blk, PCIEV_BIO_READ) < 0) {
		PCIEV_ERROR("Failed to read verify.\n");
		return;
	}

	for(offset = 0; offset < tsize; offset += sizeof(uint64_t)) {
		nowofs = toffset + offset;
		value = U64_DATA(res, nowofs);
		value ^= U64_DATA(data, nowofs);
		value ^= U64_DATA(data, nowofs + CHUNK_SIZE);
		PCIEV_DEBUG("offset=%4lld, %8llu = %8llu xor %8llu xor %8llu\n", nowofs, value, U64_DATA(res, nowofs), U64_DATA(data, nowofs), U64_DATA(data, nowofs + CHUNK_SIZE));
		U64_DATA(res, nowofs) = value;
	}

	if(pciev_submit_bio(res, toffset, tsize, pciev_vdev->bar->io_property.sector_sta, pciev_vdev->verify_blk, PCIEV_BIO_WRITE) < 0) {
		PCIEV_ERROR("Failed to write verify.\n");
		return;
	}
	
	pciev_signal_irq(0);
}

// void pciev_disptcher_calc_xor_whole(void) {
// 	int offset, idev;
// 	uint64_t value;
// 	uint8_t *data, *res;

// 	// no job, return and wait
// 	if(pciev_vdev->bar->io_property.db != DB_FREE) {
// 		return;
// 	}

// 	pciev_vdev->bar->io_property.db = DB_BUSY;

// 	// has job, now 0 to dev_cnt-1 chunks filled with data
// 	// clac res to dec_cnt

// 	data = pciev_vdev->storage_mapped;
// 	res = data + PAGE_SIZE * pciev_vdev->config.cnt_disk;

// 	for(offset = 0; offset < PAGE_SIZE; offset += sizeof(uint64_t)) {
// 		value = 0;
// 		for(idev = 0; idev < pciev_vdev->config.cnt_disk; idev ++) {
// 			value ^= *(uint64_t*)(data + PAGE_SIZE * idev + offset);
// 		}
// 		*(uint64_t*)(res + offset) = value;
// 	}

// 	pciev_vdev->bar->io_property.db = DB_DONE;
// }

static int pciev_pci_read(struct pci_bus *bus, unsigned int devfn, int where, int size, u32 *val)
{
	if (devfn != 0)
		return 1;

	memcpy(val, pciev_vdev->virtDev + where, size);

	PCIEV_DEBUG_VERBOSE("[R] 0x%x, size: %d, val: 0x%x\n", where, size, *val);

	return 0;
};

static int pciev_pci_write(struct pci_bus *bus, unsigned int devfn, int where, int size, u32 _val)
{
	u32 mask = ~(0U);
	u32 val = 0x00;
	int target = where;

	WARN_ON(size > sizeof(_val));

	memcpy(&val, pciev_vdev->virtDev + where, size);

	if (where < OFFS_PCI_PM_CAP) {
		// PCI_HDR
		if (target == PCI_COMMAND) {
			mask = PCI_COMMAND_INTX_DISABLE; // Interrupt Disable
			if ((val ^ _val) & PCI_COMMAND_INTX_DISABLE) {
				pciev_vdev->intx_disabled = !!(_val & PCI_COMMAND_INTX_DISABLE);
				if (!pciev_vdev->intx_disabled) {
					pciev_vdev->pcihdr->sts.is = 0; // only disable no enable?
				}
			}
		} else if (target == PCI_STATUS) {
			mask = 0xF200; // ?
		} else if (target == PCI_BIST) {
			mask = PCI_BIST_START; // Start BIST
		} else if (target == PCI_BASE_ADDRESS_0) {
			mask = 0xFFFFC000; // bar, lower 14 bits read only
		} else if (target == PCI_INTERRUPT_LINE) {
			mask = 0xFF; // Max latency, Min Grant, Interrupt PIN, Interrupt Line
		} else {
			mask = 0x0; // otherwise read only
		}
	} else if (where < OFFS_PCI_MSIX_CAP) {
		// PCI_PM_CAP
	} else if (where < OFFS_PCIE_CAP) {
		// PCI_MSIX_CAP
		target -= OFFS_PCI_MSIX_CAP;
		if (target == PCI_MSIX_FLAGS) {
			mask = PCI_MSIX_FLAGS_MASKALL | /* 0x4000 */
			       PCI_MSIX_FLAGS_ENABLE; /* 0x8000 */

			if ((pciev_vdev->pdev) && ((val ^ _val) & PCI_MSIX_FLAGS_ENABLE)) {
				pciev_vdev->pdev->msix_enabled = !!(_val & PCI_MSIX_FLAGS_ENABLE);
			}
		} else {
			mask = 0x0;
		}
	} else if (where < OFFS_PCI_EXT_CAP) {
		// PCIE_CAP
	} else {
		// PCI_EXT_CAP
	}
	PCIEV_DEBUG_VERBOSE("[W] 0x%x, mask: 0x%x, val: 0x%x -> 0x%x, size: %d, new: 0x%x\n", where,
			    mask, val, _val, size, (val & (~mask)) | (_val & mask));

	val = (val & (~mask)) | (_val & mask);
	memcpy(pciev_vdev->virtDev + where, &val, size);

	return 0;
};

static struct pci_ops pciev_pci_ops = {
	.read = pciev_pci_read,
	.write = pciev_pci_write,
}; // specify how to read and write PCIE configuration space

static struct pci_sysdata pciev_pci_sysdata = {
	.domain = PCIEV_PCI_DOMAIN_NUM, // PCI domain, identify host bridge number(?)
	.node = 0, // NUMA node
};

static void __dump_pci_dev(struct pci_dev *dev)
{
	/*
	PCIEV_DEBUG("bus: %p, subordinate: %p\n", dev->bus, dev->subordinate);
	PCIEV_DEBUG("vendor: %x, device: %x\n", dev->vendor, dev->device);
	PCIEV_DEBUG("s_vendor: %x, s_device: %x\n", dev->subsystem_vendor, dev->subsystem_device);
	PCIEV_DEBUG("devfn: %u, class: %x\n", dev->devfn, dev->class);
	PCIEV_DEBUG("sysdata: %p, slot: %p\n", dev->sysdata, dev->slot);
	PCIEV_DEBUG("pin: %d, irq: %u\n", dev->pin, dev->irq);
	PCIEV_DEBUG("msi: %d, msi-x:%d\n", dev->msi_enabled, dev->msix_enabled);
	PCIEV_DEBUG("resource[0]: %llx\n", pci_resource_start(dev, 0));
	*/
}

static void __init_pciev_bar(struct pci_dev *dev)
{
	struct pciev_bar *bar =
		memremap(pci_resource_start(dev, 0), PAGE_SIZE, MEMREMAP_WT);
	BUG_ON(!bar);

	PCIEV_INFO("remap bar address: %p", bar);

	pciev_vdev->bar = bar;
	memset(bar, 0x0, PAGE_SIZE);

	bar->dev_cnt = pciev_vdev->config.cnt_disk;

	// PCIEV_INFO("in bar data: 0x%llx 0x%llx.\n", bar->io_cnt, bar->storage_start, bar->storage_size);

	// pciev_vdev->dbs = ((void *)bar) + PAGE_SIZE;

	// *bar = (struct pciev_bar) {
	// 	.cap = {
	// 		.to = 1,
	// 		.mpsmin = 0,
	// 		.mqes = 1024 - 1, // 0-based value
	// 	},
	// 	.vs = {
	// 		.mjr = 1,
	// 		.mnr = 0,
	// 	},
	// };
}

static struct pci_bus *__create_pci_bus(void)
{
	struct pci_bus *bus = NULL;
	struct pci_dev *dev;

	bus = pci_scan_bus(PCIEV_PCI_BUS_NUM, &pciev_pci_ops, &pciev_pci_sysdata); // Scans the complete bus and update into the pci access structure(?)

	if (!bus) {
		PCIEV_ERROR("Unable to create PCI bus\n");
		return NULL;
	}

	/* XXX Only support a singe NVMeVirt instance in the system for now */
	list_for_each_entry(dev, &bus->devices, bus_list) {
		struct resource *res = &dev->resource[0];
		res->parent = &iomem_resource; // identify resource tree

		pciev_vdev->pdev = dev;
		dev->irq = pciev_vdev->pcihdr->intr.iline; // Interrupt Line
		// __dump_pci_dev(dev);

		__init_pciev_bar(dev);

		pciev_vdev->old_bar = kzalloc(PAGE_SIZE, GFP_KERNEL);
		PCIEV_INFO("old_bar: %p, bar: %p\n", pciev_vdev->old_bar, pciev_vdev->bar);
		BUG_ON(!pciev_vdev->old_bar && "allocating old BAR memory");
		memcpy(pciev_vdev->old_bar, pciev_vdev->bar, sizeof(*pciev_vdev->old_bar));
		// PCIEV_INFO("old_bar: 0x%llx 0x%llx, bar: 0x%llx 0x%llx\n", pciev_vdev->old_bar->io_cnt, pciev_vdev->old_bar->storage_offset, pciev_vdev->bar->io_cnt, pciev_vdev->bar->storage_offset);

		pciev_vdev->msix_table =
			memremap(pci_resource_start(pciev_vdev->pdev, 0) + PAGE_SIZE * 2,
					 NR_MAX_IO_QUEUE * PCI_MSIX_ENTRY_SIZE, MEMREMAP_WT);
		memset(pciev_vdev->msix_table, 0x00, NR_MAX_IO_QUEUE * PCI_MSIX_ENTRY_SIZE);
	}

	PCIEV_INFO("Virtual PCI bus created (node %d)\n", pciev_pci_sysdata.node);

	return bus;
};

struct pciev_dev *VDEV_INIT(void)
{
	struct pciev_dev *pciev_vdev;
	pciev_vdev = kzalloc(sizeof(*pciev_vdev), GFP_KERNEL);

	pciev_vdev->virtDev = kzalloc(PAGE_SIZE, GFP_KERNEL);

	pciev_vdev->pcihdr = pciev_vdev->virtDev + OFFS_PCI_HDR;
	pciev_vdev->pmcap = pciev_vdev->virtDev + OFFS_PCI_PM_CAP;
	pciev_vdev->msixcap = pciev_vdev->virtDev + OFFS_PCI_MSIX_CAP;
	pciev_vdev->pciecap = pciev_vdev->virtDev + OFFS_PCIE_CAP;
	pciev_vdev->extcap = pciev_vdev->virtDev + OFFS_PCI_EXT_CAP;

	return pciev_vdev;
}

void VDEV_FINALIZE(struct pciev_dev *pciev_vdev)
{
	if (pciev_vdev->msix_table)
		memunmap(pciev_vdev->msix_table);

	if (pciev_vdev->bar)
		memunmap(pciev_vdev->bar);

	// if (pciev_vdev->old_bar)
	// 	kfree(pciev_vdev->old_bar);

	// if (pciev_vdev->old_dbs)
	// 	kfree(pciev_vdev->old_dbs);

	if (pciev_vdev->virtDev)
		kfree(pciev_vdev->virtDev);

	if (pciev_vdev)
		kfree(pciev_vdev);
}

static void PCI_HEADER_SETTINGS(struct pci_header *pcihdr, unsigned long base_pa)
{
	pcihdr->id.did = PCIEV_DEVICE_ID;
	pcihdr->id.vid = PCIEV_VENDOR_ID;
	/*
	pcihdr->cmd.id = 1;
	pcihdr->cmd.bme = 1;
	*/
	pcihdr->cmd.mse = 1; // device can respond to Memory Space accesses
	pcihdr->sts.cl = 1;	 // the device implements the pointer for a New Capabilities Linked list at offset 0x34

	pcihdr->htype.mfd = 0; // header type, 0 refer to Endpoints in PCIe
	pcihdr->htype.hl = PCI_HEADER_TYPE_NORMAL;

	pcihdr->rid = 0x01; // revision ID, Specifies a revision identifier for a particular device. Where valid IDs are allocated by the vendor.

	// pcihdr->cc.bcc = PCI_BASE_CLASS_STORAGE; // Mass Storage Controller
	// pcihdr->cc.scc = 0x08;					 // Non-Volatile Memory Controller
	// pcihdr->cc.pi = 0x02; // NVM Express

	// pcihdr->cc.bcc = PCI_BASE_CLASS_STORAGE; // Mass Storage Controller
	// pcihdr->cc.scc = 0x06;					 // Serial ATA Controller
	// pcihdr->cc.pi = 0x01;					 // AHCI 1.0

	pcihdr->cc.bcc = 0x00;
	pcihdr->cc.scc = 0x00;
	pcihdr->cc.pi = 0x00;

	pcihdr->mlbar.tp = PCI_BASE_ADDRESS_MEM_TYPE_64 >> 1; // the base register is 64-bits wide and can be mapped anywhere in the 64-bit Memory Space
	pcihdr->mlbar.ba = (base_pa & 0xFFFFFFFF) >> 14; // minimum address space is 2^14 = 16KB

	pcihdr->mulbar = base_pa >> 32;

	pcihdr->ss.ssid = PCIEV_SUBSYSTEM_ID;
	pcihdr->ss.ssvid = PCIEV_SUBSYSTEM_VENDOR_ID;

	pcihdr->erom = 0x0; // disable expansion ROM

	pcihdr->cap = OFFS_PCI_PM_CAP; // power management capability

	pcihdr->intr.ipin = 0;
	pcihdr->intr.iline = PCIEV_INTX_IRQ;
}

static void PCI_PMCAP_SETTINGS(struct pci_pm_cap *pmcap)
{
	pmcap->pid.cid = PCI_CAP_ID_PM; // indicates that the data structure currently being pointed to is the PCI Power Management data structure
	pmcap->pid.next = OFFS_PCI_MSIX_CAP; // describes the location of the next item in the function’s capability list

	pmcap->pc.vs = 3; // 011b indicates that this function complies with revision 1.2 of the PCI Power Management	Interface Specification.pmcap->pmcs.nsfrst = 1;
	pmcap->pmcs.ps = PCI_PM_CAP_PME_D0 >> 16; // current power state is D0
}

static void PCI_MSIXCAP_SETTINGS(struct pci_msix_cap *msixcap)
{
	msixcap->mxid.cid = PCI_CAP_ID_MSIX;
	msixcap->mxid.next = OFFS_PCIE_CAP;

	msixcap->mxc.ts = 127; // 0x80, encoded as n-1
	msixcap->mxc.mxe = 1; // enable MSI-X

	msixcap->mtab.tbir = 0;
	msixcap->mtab.to = 0x400;

	msixcap->mpba.pbir = 0;
	msixcap->mpba.pbao = 0x1000;
}

static void PCI_PCIECAP_SETTINGS(struct pcie_cap *pciecap)
{
	pciecap->pxid.cid = PCI_CAP_ID_EXP;
	pciecap->pxid.next = 0x0;

	pciecap->pxcap.ver = PCI_EXP_FLAGS;
	pciecap->pxcap.imn = 0;
	pciecap->pxcap.dpt = PCI_EXP_TYPE_ENDPOINT; // endpoint function

	pciecap->pxdcap.mps = 1; // 256 bytes max payload size
	pciecap->pxdcap.pfs = 0; // No Function Number bits are used for Phantom Functions
	pciecap->pxdcap.etfs = 1; // 8-bit Tag field supported
	pciecap->pxdcap.l0sl = 6; // Maximum of 4 μs
	pciecap->pxdcap.l1l = 2;  // Maximum of 4 μs
	pciecap->pxdcap.rer = 1; // enable
	pciecap->pxdcap.csplv = 0;
	pciecap->pxdcap.cspls = 0; // 1.0x
	pciecap->pxdcap.flrc = 1;  // the Function supports the optional Function Level Reset mechanism
}

static void PCI_EXTCAP_SETTINGS(struct pci_ext_cap *ext_cap)
{
	off_t offset = 0;
	void *ext_cap_base = ext_cap;

	/* AER */
	ext_cap->cid = PCI_EXT_CAP_ID_ERR;
	ext_cap->cver = 1;
	ext_cap->next = PCI_CFG_SPACE_SIZE + 0x50;

	ext_cap = ext_cap_base + 0x50;
	ext_cap->cid = PCI_EXT_CAP_ID_VC;
	ext_cap->cver = 1;
	ext_cap->next = PCI_CFG_SPACE_SIZE + 0x80;

	ext_cap = ext_cap_base + 0x80;
	ext_cap->cid = PCI_EXT_CAP_ID_PWR;
	ext_cap->cver = 1;
	ext_cap->next = PCI_CFG_SPACE_SIZE + 0x90;

	ext_cap = ext_cap_base + 0x90;
	ext_cap->cid = PCI_EXT_CAP_ID_ARI;
	ext_cap->cver = 1;
	ext_cap->next = PCI_CFG_SPACE_SIZE + 0x170;

	ext_cap = ext_cap_base + 0x170;
	ext_cap->cid = PCI_EXT_CAP_ID_DSN;
	ext_cap->cver = 1;
	ext_cap->next = PCI_CFG_SPACE_SIZE + 0x1a0;

	ext_cap = ext_cap_base + 0x1a0;
	ext_cap->cid = PCI_EXT_CAP_ID_SECPCI;
	ext_cap->cver = 1;
	ext_cap->next = 0;

	/*
	*(ext_cap + 1) = (struct pci_ext_cap) {
		.id = {
			.cid = 0xdead,
			.cver = 0xc,
			.next = 0xafe,
		},
	};

	PCI_CFG_SPACE_SIZE + ...;

	ext_cap = ext_cap + ...;
	ext_cap->id.cid = PCI_EXT_CAP_ID_DVSEC;
	ext_cap->id.cver = 1;
	ext_cap->id.next = 0;
	*/
}

bool PCIEV_PCI_INIT(struct pciev_dev *pciev_vdev)
{
	PCI_HEADER_SETTINGS(pciev_vdev->pcihdr, pciev_vdev->config.memmap_start);
	PCI_PMCAP_SETTINGS(pciev_vdev->pmcap);
	PCI_MSIXCAP_SETTINGS(pciev_vdev->msixcap);
	PCI_PCIECAP_SETTINGS(pciev_vdev->pciecap);
	PCI_EXTCAP_SETTINGS(pciev_vdev->extcap);

	pciev_vdev->intx_disabled = false;

	pciev_vdev->virt_bus = __create_pci_bus();
	if (!pciev_vdev->virt_bus)
		return false;

	return true;
}
