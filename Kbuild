obj-m   := graid.o
graid-objs := main.o pciedrv.o block.o pci.o device.o
obj-m	+= biotest/ readtest/
ccflags-y := -DCONFIG_GRAID_DEBUG