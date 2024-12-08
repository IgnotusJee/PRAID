#ifndef _LIB_DEVICE_H
#define _LIB_DEVICE_H

#include <linux/pci.h>
#include <linux/msi.h>
#include <asm/apic.h>
#include <linux/types.h>
#include <linux/time.h>

#define PCIEV_DRV_NAME "PRAID_DEVICE"

#ifdef CONFIG_PRAID_DEBUG
#define PCIEV_DEBUG(string, args...) printk(KERN_DEBUG "%s %s: " string, PCIEV_DRV_NAME, __func__, ##args)
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

// 200s 未更改的数据就标记为冷数据
#define DOORMAT_TIME_SEC 3

// include latest update time in secends, 0 for no dirty data
struct __packed chunk_info {
    time64_t update_sec;
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

	struct task_struct *pciev_dispatcher;

	void *storage_mapped;

	void __iomem *msix_table;

	bool intx_disabled;

	struct pciev_bar *old_bar;
	struct pciev_bar __iomem *bar;
	struct chunk_info __iomem *si_start;

	void* buffer;
	struct praid_dev *gdev;
};


extern struct pciev_dev *pciev_vdev;
struct pciev_dev *VDEV_INIT(void);
void VDEV_FINALIZE(struct pciev_dev *pciev_vdev);
void pciev_proc_bars(void);
void pciev_storage_dispatch(void);
bool PCIEV_PCI_INIT(struct pciev_dev *dev);

int PCIEV_init(struct praid_dev* gdev);
void PCIEV_exit(void);

#endif /* _LIB_DEVICE_H */