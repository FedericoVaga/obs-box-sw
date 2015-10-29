# include parent_common.mk for buildsystem's defines
# use absolute path for REPO_PARENT
CURDIR:=$(shell /bin/pwd)
REPO_PARENT=$(CURDIR)/../..
-include $(REPO_PARENT)/parent_common.mk

LINUX ?= /lib/modules/$(shell uname -r)/build
ZIO ?= ../zio
FMC_BUS ?= ../fmc-bus
SPEC_SW ?= ../spec-sw

# FMC_BUS_ABS, ZIO_ABS and SPEC_SW_ABS has to be absolut path,
# due to beeing passed to the Kbuild
FMC_BUS_ABS ?= $(abspath $(FMC_BUS) )
ZIO_ABS ?= $(abspath $(ZIO) )
SPEC_SW_ABS ?= $(abspath $(SPEC_SW) )

GIT_VERSION = $(shell git describe --always --dirty --long --tags)
export GIT_VERSION

ZIO_VERSION = $(shell cd $(ZIO_ABS); git describe --always --dirty --long --tags)
export ZIO_VERSION

all modules:
	$(MAKE) -C $(LINUX) M=$(CURDIR) FMC_BUS_ABS=$(FMC_BUS_ABS) \
			ZIO_ABS=$(ZIO_ABS) SPEC_SW_ABS=$(SPEC_SW_ABS) \
			modules

install modules_install: modules
	$(MAKE) -C $(LINUX) M=$(CURDIR) modules_install

# be able to run the "clean" rule even if $(LINUX) is not valid
clean:
	rm -rf *.o *~  .*.cmd *.ko *.mod.c .tmp_versions Module.symvers \
		Module.markers modules.order
