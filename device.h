#ifndef _LIB_DEVICE_H
#define _LIB_DEVICE_H

#include <linux/pci.h>
#include <linux/msi.h>
#include <asm/apic.h>
#include <linux/types.h>

#define PCIEV_DRV_NAME "GRAID_DEVICE"

#ifdef CONFIG_PCIEV_DEBUG
#define PCIEV_DEBUG(string, args...) printk(KERN_INFO "%s: " string, PCIEV_DRV_NAME, ##args)
#ifdef CONFIG_PCIEV_DEBUG_VERBOSE
#define PCIEV_DEBUG_VERBOSE(string, args...) printk(KERN_INFO "%s: " string, PCIEV_DRV_NAME, ##args)
#else
#define PCIEV_DEBUG_VERBOSE(string, args...)
#endif
#else
#define PCIEV_DEBUG(string, args...)
#define PCIEV_DEBUG_VERBOSE(string, args...)
#endif

#define PCIEV_INFO(string, args...) printk(KERN_INFO "%s: " string, PCIEV_DRV_NAME, ##args)
#define PCIEV_ERROR(string, args...) printk(KERN_ERR "%s: " string, PCIEV_DRV_NAME, ##args)
#define PCIEV_ASSERT(x) BUG_ON((!(x)))

#define NR_MAX_IO_QUEUE 72
#define NR_MAX_PARALLEL_IO 16384

#define PCIEV_INTX_IRQ 15

#define KB(k) ((k) << 10)
#define MB(m) ((m) << 20)
#define GB(g) ((g) << 30)

#define BYTE_TO_KB(b) ((b) >> 10)
#define BYTE_TO_MB(b) ((b) >> 20)
#define BYTE_TO_GB(b) ((b) >> 30)

struct pciev_config {
	unsigned long memmap_start; // byte
	unsigned long memmap_size; // byte

	unsigned long storage_start; //byte
	unsigned long storage_size; // byte

	unsigned int cnt_disk;

	unsigned int cpu_nr_dispatcher;
};

struct pciev_dev {
	struct pci_bus *virt_bus;
	void *virtDev;
	struct pci_header *pcihdr;
	struct pci_pm_cap *pmcap;
	struct pci_msix_cap *msixcap;
	struct pcie_cap *pciecap;
	struct pci_ext_cap *extcap;

	struct pci_dev *pdev;

	struct pciev_config config;
	struct task_struct *pciev_dispatcher;

	void *storage_mapped;

	void __iomem *msix_table;

	bool intx_disabled;

	struct pciev_bar *old_bar;
	struct pciev_bar __iomem *bar;

	struct block_device *verify_blk;
};

extern struct pciev_dev *pciev_vdev;
struct pciev_dev *VDEV_INIT(void);
void VDEV_FINALIZE(struct pciev_dev *pciev_vdev);
void pciev_proc_bars(void);
void pciev_dispatcher_clac_xor_single(void);
bool PCIEV_PCI_INIT(struct pciev_dev *dev);

extern unsigned long memmap_start;
extern unsigned long memmap_size;
extern unsigned int cpu;

int PCIEV_init(struct block_device*, unsigned int cnt_dev);
void PCIEV_exit(void);

#endif /* _LIB_DEVICE_H */