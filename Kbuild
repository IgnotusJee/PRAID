obj-m   := nraid.o
nraid-objs := main.o pciedrv.o block.o pci.o device.o
obj-m	+= biotest/ readtest/
ccflags-y := -DCONFIG_NRAID_DEBUG