## Version 0.3.0 - 2026-11-08

- Verilator cores: `nvcx::write_shell -verilator ARGS` verilates a Verilog
  module (once per generic set) and the bridge simulates it alongside xsim
  cores and NVC's own (`src/verilator`, `nvcx::verilate`). Models are 2-state
  and settle a time slot at once, so a Verilator core must not drive a clock
  NVC uses (README, `man nvcx`). `tests/test_verilator.py` runs without
  Vivado.
- The bridge reaches cores through a backend-neutral interface
  (`nvcx::Core`, `include/nvcx/core.hpp`); xsim (`src/xsim/`) is one
  backend, chosen by what a snapshot directory holds. No change in
  behaviour.
- Releases carry a Verilator package (5.054+git202611072109.74cf7aa-1+nvcx2,
  from `contrib/verilator`: Debian's packaging, an upstream snapshot, and
  the patches nvcx needs). nvcx's package depends on it, and the Docker
  image installs it.

## Version 0.2.0 - 2026-10-26

First release.

- Cores simulated by AMD/Xilinx xsim (encrypted IP, XPM macros, and
  SystemVerilog NVC does not yet support) run inside an NVC simulation,
  each behind a generated VHDL shell, through a VHPI plugin
  (`nvc --load`).
- `nvcx::write_shell` (a Tcl package, run with `nvc --do`) writes a
  component's shell. Given a library, it elaborates an xsim snapshot for
  each set of generics the design uses, cached between runs; given a
  snapshot elaborated already, it uses that one.
- NVC and each core advance in lockstep, one delta cycle at a time, so
  delta ordering across the boundary is as in a single simulator.
- Ports may be `std_logic` or `bit` scalars and arrays, `integer`, `real`
  or `boolean`, on VHDL and Verilog cores (with 4-state values).
- A core's reports and assertions become NVC's, at their severity.
- All instances share one xsim kernel.
- Vivado 2023.2, 2024.1, 2025.1, 2025.2 and 2026.1 are supported. Older
  releases are refused: their XSI cannot open a snapshot outside the
  working directory.
- nvcx needs NVC 1.23.0 with the patches in `contrib/`, which are
  submitted upstream.
- Debian 13, Ubuntu 24.04 and Ubuntu 26.04 packages are attached to each
  release, with an NVC package carrying those patches (1.23.0-1+nvcx1)
  until an NVC release includes them.
