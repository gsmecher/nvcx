# The project's Docker image, and how to run make in it against the Vivado that
# XILINX_VIVADO names. Included by tests/, hello/ and bench/ from outside the
# container.
#
# Vivado is bind-mounted read-only at its own path. nvcx doesn't link against
# it, so one image and one build serve every supported release. HOME is
# scratch inside the container (Vivado writes there), so licences in
# ~/.Xilinx are offered through XILINXD_LICENSE_FILE instead. Simulation
# doesn't need one, but hello/'s IP generation does (as of 2026.1).

ifeq ($(XILINX_VIVADO),)
  $(error XILINX_VIVADO is not set)
endif

NVCX_ROOT   := $(realpath $(dir $(lastword $(MAKEFILE_LIST))))
XILINX_ROOT := $(realpath $(XILINX_VIVADO)/../..)
IMAGE       := nvcx

LICENSES := $(if $(wildcard $(HOME)/.Xilinx),/licenses)
space := $(empty) $(empty)
LICENSE_PATH := $(subst $(space),:,$(strip $(XILINXD_LICENSE_FILE) $(LICENSES)))

# The includer's directory, as seen inside the container
DOCKER_WORKDIR ?= $(patsubst $(NVCX_ROOT)%,/work%,$(CURDIR))

DOCKER_BUILD = docker build -t $(IMAGE) -f $(NVCX_ROOT)/Dockerfile $(NVCX_ROOT)

# Extra `docker run` options go in DOCKER_ARGS
# --init: a real init as PID 1, which reaps orphans and forwards signals, so
# that an interrupted run tears down instead of leaving make spinning
DOCKER_RUN = docker run --rm --init --network=host \
	-u $(shell id -u):$(shell id -g) \
	-v $(NVCX_ROOT):/work -w $(DOCKER_WORKDIR) \
	-v $(XILINX_ROOT):$(XILINX_ROOT):ro \
	$(if $(LICENSES),-v $(HOME)/.Xilinx:$(LICENSES):ro) \
	$(if $(LICENSE_PATH),-e XILINXD_LICENSE_FILE=$(LICENSE_PATH)) \
	-e XILINX_VIVADO=$(XILINX_VIVADO) -e HOME=/tmp \
	-e PATH=$(XILINX_VIVADO)/bin:/usr/local/bin:/usr/bin:/bin \
	$(DOCKER_ARGS) $(IMAGE)

# Each target in DOCKER_TARGETS builds the image and reruns make inside it
.PHONY: $(DOCKER_TARGETS)
$(DOCKER_TARGETS):
	$(DOCKER_BUILD)
	$(DOCKER_RUN) make $@
