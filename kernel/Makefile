LINUX ?= /lib/modules/$(shell uname -r)/build
ZIO ?= $(src)/../zio
SPEC_SW ?= $(src)/../spec-sw
FMC_BUS ?= $(src)/../fmc-bus

GIT_VERSION = $(shell cd $(src); git describe --always --dirty --long --tags)

all modules:
	$(MAKE) -C $(LINUX) M=$(shell /bin/pwd) modules

install modules_install:
	$(MAKE) -C $(LINUX) M=$(shell /bin/pwd) modules_install

# be able to run the "clean" rule even if $(LINUX) is not valid
clean:
	rm -rf *.o *~  .*.cmd *.ko *.mod.c .tmp_versions Module.symvers \
		Module.markers modules.order
