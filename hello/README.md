# Hello, world: Embedding Vivado XSIM inside NVC

A VHDL testbench, simulated by NVC, drives a Xilinx DDS Compiler core (which is
encrypted) and an `xpm_memory_sdpram` (which is SystemVerilog with generics).
Both of these are currently out of NVC's reach. Instead, we use NVC's VHPI
interface and embed a xsim kernel using its XSI API.

    $ export XILINX_VIVADO=/opt/xilinx/2026.1/Vivado
    $ make
    $ surfer tb.fst

For portability's sake, this example uses Docker to insulate the runtime from
the host OS. `tb.fst` holds NVC's waveform, including the shell's ports.

To run it on the host instead, with the NVC that nvcx needs (see the top-level
README), CMake, Boost.Context and tcllib installed:

    $ make DOCKER=0

Either way, the Makefile builds nvcx from this tree, installs it into
`prefix/` here, and finds its pieces there through pkg-config, as your own
build would with an installed nvcx.

# Why?

* NVC can now simulate encrypted IP - this has been a longstanding and
  fundamental blocker with open-source RTL simulators, which (for obvious and
  fundamental reasons) can't make use of IEEE 1735-style encrypted IP.

* xsim is now a "polyfill" for NVC - a SystemVerilog design that's beyond NVC's
  current capabilities (e.g. AMD/Xilinx xpm, which use entity-level generics)
  can now instantiate these IP blocks in xsi and simulate them as black boxes.

* Similarly, NVC can be a "polyfill" for xsim - when using NVC as a top-level
  simulation kernel and xsim for the "mop-up" terminal IPs, I can make use of
  VHPI, PSL, FST/VCD, cocotb, et cetera.
