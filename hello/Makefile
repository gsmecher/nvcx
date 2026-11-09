# Hello, world: an encrypted Xilinx DDS core simulated by xsim, inside an NVC
# simulation.
#
#   $ source /opt/xilinx/2026.1/Vivado/settings64.sh  # or whatever
#   $ make
#
# By default this runs in the project's Docker image (with Vivado bind-mounted),
# to avoid too much dependence on the host OS/runtime. To run it on the host
# instead, which then needs NVC (with the patches in ../contrib), CMake,
# Boost.Context and tcllib:
#
#   $ make DOCKER=0

ifeq ($(XILINX_VIVADO),)
  $(error XILINX_VIVADO is not set)
endif

# The nvcx source tree. (The test suite runs a copy of this directory from
# elsewhere, against the tree itself: nothing here depends on what a copy of
# the tree would have to include.)
NVCX_ROOT ?= ..

# Inside the container, do the thing; outside, only if asked to
ifneq ($(wildcard /.dockerenv),)
  override DOCKER := 0
endif
DOCKER ?= 1

ifeq ($(DOCKER),1)
#
# Build the image and re-run this Makefile inside it
#

DOCKER_TARGETS := all clean sim snapshot bridge
include $(NVCX_ROOT)/docker.mk

else
#
# Do the thing
#

.PHONY: all tools bridge snapshot sim clean
all: sim

# Vivado's tools come from XILINX_VIVADO, whatever PATH holds
export PATH := $(XILINX_VIVADO)/bin:$(PATH)

# 0. What the steps below run: say so now, not half-way through
tools:
	@for t in cmake pkg-config nvc vivado xelab; do \
		command -v $$t >/dev/null || { echo "hello: $$t not found on PATH" >&2; exit 1; }; \
	done

# 1. Build and install nvcx into a local prefix. To use an nvcx that's already
#    installed, drop `bridge` from sim's prerequisites below: pkg-config falls
#    back to its default search path.
PREFIX := $(CURDIR)/prefix
PKG_CONFIG := PKG_CONFIG_PATH=$(PREFIX)/lib/pkgconfig pkg-config
# (CMAKE_INSTALL_LIBDIR: lib64 on some hosts, by default)
bridge: tools
	cmake -S $(NVCX_ROOT) -B build -DCMAKE_INSTALL_PREFIX=$(PREFIX) -DCMAKE_INSTALL_LIBDIR=lib
	cmake --build build -j
	cmake --install build

# Where the installed pieces are (deferred: they exist only after `bridge`)
NVCX_PLUGIN = $(shell $(PKG_CONFIG) --variable=plugin nvcx)
NVCX_VHDL   = $(shell $(PKG_CONFIG) --variable=vhdl nvcx)
NVCX_TCLDIR = $(shell $(PKG_CONFIG) --variable=tcldir nvcx)

# 2. Vivado
EXPORT   := export_sim/hello_dds/xsim
SNAPSHOT := $(EXPORT)/xsim.dir/hello_dds/xsimk.so
snapshot: $(SNAPSHOT)
$(SNAPSHOT): generate_ip.tcl | tools
	./generate_ip.tcl
	sed -i '/^  xelab -dll /! s/^  xelab /  xelab -dll /' $(EXPORT)/hello_dds.sh
	cd $(EXPORT) && ./hello_dds.sh -step compile
	cd $(EXPORT) && ./hello_dds.sh -step elaborate

# 3. NVC
XPM := $(XILINX_VIVADO)/data/ip/xpm/xpm_VCOMP.vhd
NVC := nvc --std=2008 --map=nvcx:nvcx --map=xpm:xpm
sim: bridge $(SNAPSHOT)
	# Teach your preferred build tool how to do it better
	@test -n "$(NVCX_PLUGIN)" || { echo "hello: pkg-config cannot find nvcx (looked in $(PREFIX)/lib/pkgconfig first)" >&2; exit 1; }
	rm -rf work
	$(NVC) --work=nvcx:nvcx -a $(NVCX_VHDL)
	$(NVC) --work=xpm:xpm -a $(XPM)
	$(NVC) -a tb.vhd -e tb # first pass: elaborate to identify XSI-shaped holes
	TCLLIBPATH=$(NVCX_TCLDIR) ./generate_shells.tcl # xelab (for xsim) and generate RTL wrappers (for nvc)
	$(NVC) -a *_shell.vhd -e tb # second pass: elaborate everything
	$(NVC) -r --load $(NVCX_PLUGIN) --wave=tb.fst tb

clean:
	rm -rf vivado export_sim \
		*_shell.vhd \
		work nvcx xpm \
		tb.fst \
		xsim.wdb \
		nvcx.work \
		build prefix \
		.Xil \
		xsim.dir \
		*.log *.jou *.pb
endif
