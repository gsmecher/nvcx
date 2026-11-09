# Use `make test` at the top level

# The container builds into build-docker/ on the host, mounted over build/, so its binaries never
# mix with a native build (the hello test builds its own, in a copy of hello/ under pytest's tmp).
DOCKER_WORKDIR := /work
include ../docker.mk
DOCKER_ARGS = -v $(NVCX_ROOT)/build-docker:/work/build

.PHONY: image test test-all shell
image:
	$(DOCKER_BUILD)
	mkdir -p $(NVCX_ROOT)/build-docker $(NVCX_ROOT)/build # or Docker creates them root-owned

test: image
	$(DOCKER_RUN) make pytest

# All "blessed" Vivado releases, installed as <root>/<release>/Vivado (2025.1 and
# later) or <root>/Vivado/<release>
VIVADOES ?= 2023.2 2024.1 2025.1 2025.2 2026.1
test-all:
	@fail=; for v in $(VIVADOES); do \
		echo "=== $$v"; \
		d=$(XILINX_ROOT)/$$v/Vivado; test -d $$d || d=$(XILINX_ROOT)/Vivado/$$v; \
		XILINX_VIVADO=$$d $(MAKE) --no-print-directory test || fail="$$fail $$v"; \
	done; \
	test -z "$$fail" || { echo "FAILED:$$fail"; exit 1; }

shell: DOCKER_ARGS += -it
shell: image
	$(DOCKER_RUN) bash
