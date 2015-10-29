# include parent_common.mk for buildsystem's defines
# use absolute path for REPO_PARENT
REPO_PARENT=$(shell /bin/pwd)/..
-include $(REPO_PARENT)/parent_common.mk

.PHONY: all clean modules install modules_install clean_all
.PHONY: gitmodules prereq prereq_install prereq_install_warn prereq_clean

DIRS = kernel tools

all clean modules install modules_install: gitmodules
	@if echo $@ | grep -q install; then $(MAKE) prereq_install_warn; fi
	for d in $(DIRS); do $(MAKE) -C $$d $@ || exit 1; done

all modules: prereq

clean_all: clean prereq_clean

#### The following targets are used to manage prerequisite repositories
#### only for THIS repository
gitmodules:
	@test -d fmc-bus/doc || echo "Checking out submodules"
	@test -d fmc-bus/doc || git submodule update --init

# The user can override, using environment variables, the place for our
# three submodules. Note that svec-sw is not built, as it uses cern-internal
# pathnames, and thus won't build elsewhere. We have it as a submodule to
# find needed headers to build kernel code.
#
FMC_BUS ?= fmc-bus
ZIO ?= zio
SPEC_SW ?= spec-sw
# Use the absolute path so it can be used by submodule
# FMC_BUS_ABS, ZIO_ABS and SPEC_SW_ABS has to be absolut path,
# due to beeing passed to the Kbuild
FMC_BUS_ABS ?= $(abspath $(FMC_BUS) )
ZIO_ABS ?= $(abspath $(ZIO) )
SPEC_SW_ABS ?= $(abspath $(SPEC_SW) )

export FMC_BUS_ABS
export ZIO_ABS
export SPEC_SW_ABS

SUBMOD = $(FMC_BUS_ABS) $(ZIO_ABS) $(SPEC_SW_ABS)

prereq:
	for d in $(SUBMOD); do $(MAKE) -C $$d || exit 1; done

prereq_install_warn:
	@test -f .prereq_installed || \
		echo -e "\n\n\tWARNING: Consider \"make prereq_install\"\n"

prereq_install:
	for d in $(SUBMOD); do $(MAKE) -C $$d modules_install || exit 1; done
	touch .prereq_installed

prereq_clean:
	for d in $(SUBMOD); do $(MAKE) -C $$d clean || exit 1; done
