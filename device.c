#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "device.h"
#include "graid.h"

struct pciev_dev *pciev_vdev = NULL;

static int pciev_dispatcher(void *data) {
	PCIEV_INFO("pciev_dispatcher started on cpu %d (node %d)\n",
			   pciev_vdev->gdev->config.cpu_nr_dispatcher,
			   cpu_to_node(pciev_vdev->gdev->config.cpu_nr_dispatcher));
	
	while (!kthread_should_stop()) {
		pciev_proc_bars();
		pciev_storage_dispatch();
		cond_resched();
	}

	return 0;
}

static void PCIEV_DISPATCHER_INIT(struct pciev_dev *pciev_vdev)
{
	pciev_vdev->pciev_dispatcher = kthread_create(pciev_dispatcher, NULL, "pciev_dispatcher");
	kthread_bind(pciev_vdev->pciev_dispatcher, pciev_vdev->gdev->config.cpu_nr_dispatcher);
	wake_up_process(pciev_vdev->pciev_dispatcher);
}

static void PCIEV_DISPATCHER_FINAL(struct pciev_dev *pciev_vdev)
{
	if (!IS_ERR_OR_NULL(pciev_vdev->pciev_dispatcher)) {
		kthread_stop(pciev_vdev->pciev_dispatcher);
		pciev_vdev->pciev_dispatcher = NULL;
	}
}

static void PCIEV_STORAGE_INIT(struct pciev_dev *pciev_vdev) {
	PCIEV_INFO("Storage: %#010lx-%#010lx (%lu MiB)\n",
			   pciev_vdev->gdev->config.storage_start,
			   pciev_vdev->gdev->config.storage_start + pciev_vdev->gdev->config.storage_size,
			   BYTE_TO_MB(pciev_vdev->gdev->config.storage_size));

	pciev_vdev->storage_mapped = memremap(pciev_vdev->gdev->config.storage_start,
										  pciev_vdev->gdev->config.storage_size, MEMREMAP_WB);

	if (pciev_vdev->storage_mapped == NULL)
		PCIEV_ERROR("Failed to map storage memory.\n");

	pciev_vdev->si_start = pciev_vdev->storage_mapped;
	memset(pciev_vdev->si_start, 0, sizeof(struct chunk_info) * (pciev_vdev->gdev->size >> CHUNK_SHIFT));

	pciev_vdev->buffer = kzalloc(CHUNK_SIZE*2, GFP_KERNEL);
}

static void PCIEV_STORAGE_FINAL(struct pciev_dev *pciev_vdev) {
	if (pciev_vdev->storage_mapped)
		memunmap(pciev_vdev->storage_mapped);
	if(pciev_vdev->buffer)
		kfree(pciev_vdev->buffer);
}

int PCIEV_init(struct graid_dev* gdev) {
	pciev_vdev = VDEV_INIT();
	if (!pciev_vdev)
		return -EINVAL;

	pciev_vdev->gdev = gdev;

	PCIEV_STORAGE_INIT(pciev_vdev);

	if (!PCIEV_PCI_INIT(pciev_vdev)) {
		goto ret_err;
	}

	PCIEV_DISPATCHER_INIT(pciev_vdev);

	pci_bus_add_devices(pciev_vdev->virt_bus);

	PCIEV_INFO("Virtual PCIE device created\n");

	return 0;

ret_err:
	VDEV_FINALIZE(pciev_vdev);
	return -EIO;
}

void PCIEV_exit(void) {

	if (pciev_vdev->virt_bus != NULL) {
		pci_stop_root_bus(pciev_vdev->virt_bus);
		pci_remove_root_bus(pciev_vdev->virt_bus);
	}

	PCIEV_DISPATCHER_FINAL(pciev_vdev);

	PCIEV_STORAGE_FINAL(pciev_vdev);
	VDEV_FINALIZE(pciev_vdev);

	PCIEV_INFO("Virtual PCIE device closed\n");
}