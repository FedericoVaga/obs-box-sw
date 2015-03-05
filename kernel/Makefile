LINUX ?= /lib/modules/$(shell uname -r)/build
ZIO ?= $(src)/../zio
SPEC_SW ?= $(src)/../spec-sw
FMC_BUS ?= $(src)/../fmc-bus

KBUILD_EXTRA_SYMBOLS := \
	$(ZIO)/Module.symvers \
	$(FMC_BUS)/kernel/Module.symvers

GIT_VERSION = $(shell cd $(src); git describe --always --dirty --long --tags)

# For this CSM_VERSION, please see ohwr.org/csm documentation
ifdef CONFIG_CSM_VERSION
  ccflags-y += -D"CERN_SUPER_MODULE=MODULE_VERSION(\"$(CONFIG_CSM_VERSION)\")"
else
  ccflags-y += -DCERN_SUPER_MODULE=""
endif

ccflags-y += -DGIT_VERSION=\"$(GIT_VERSION)\" \
	-I$(ZIO)/include \
	-I$(FMC_BUS)/kernel/include \
	-I$(SPEC_SW)/kernel \
	-I$(src)
ccflags-$(CONFIG_OBS_BOX_DEBUG) += -DDEBUG
ccflags-$(CONFIG_OBS_BOX_VERBOSE_DEBUG) += -DVERBOSE_DEBUG

obj-m := obs-box.o
obs-box-y = obsbox-zio.o
obs-box-y += obsbox-irq.o
obs-box-y += obsbox-fmc.o
obs-box-y += obsbox-regtable.o

all modules:
	$(MAKE) -C $(LINUX) M=$(shell /bin/pwd) modules

install modules_install:
	$(MAKE) -C $(LINUX) M=$(shell /bin/pwd) modules_install

# be able to run the "clean" rule even if $(LINUX) is not valid
clean:
	rm -rf *.o *~  .*.cmd *.ko *.mod.c .tmp_versions Module.symvers \
		Module.markers modules.order
