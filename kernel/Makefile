# include parent_common.mk for buildsystem's defines
# use absolute path for REPO_PARENT
REPO_PARENT=$(shell /bin/pwd)/../..
-include $(REPO_PARENT)/parent_common.mk

LINUX ?= /lib/modules/$(shell uname -r)/build
ZIO ?= ../zio
FMC_BUS ?= ../fmc-bus
SPEC_SW ?= ../spec-sw

export ZIO
export FMC_BUS
export SPEC_SW

GIT_VERSION = $(shell git describe --always --dirty --long --tags)
export GIT_VERSION

ZIO_VERSION = $(shell cd $(ZIO); git describe --always --dirty --long --tags)
export ZIO_VERSION


all modules:
	$(MAKE) -C $(LINUX) M=$(shell /bin/pwd) modules

install modules_install:
	$(MAKE) -C $(LINUX) M=$(shell /bin/pwd) modules_install

# be able to run the "clean" rule even if $(LINUX) is not valid
clean:
	rm -rf *.o *~  .*.cmd *.ko *.mod.c .tmp_versions Module.symvers \
		Module.markers modules.order
