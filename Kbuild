obj-m   := praid.o
praid-objs := main.o pciedrv.o block.o pci.o device.o
obj-m	+= biotest/ readtest/
ccflags-y := -DCONFIG_PRAID_DEBUG