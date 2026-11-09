# nvcx

Embed AMD/Xilinx xsim inside [NVC](https://www.nickg.me.uk/nvc/), a fast,
modern, open-source VHDL simulator with improving support for SystemVerilog. By
doing this, encrypted IP and SystemVerilog NVC doesn't yet support can be
simulated reliably in an NVC "world", removing a major feature gap in
open-source RTL simulation.

Cores delegated to xsim become a VHDL shell and a VHPI back-end containing
xsim's simulation model. Because NVC simulates most of the design, non-xsim
parts of the world (ideally, most of it) can freely use VHPI, PSL, FST/VDB, et
cetera. xsim only exists to plug encrypted-IP and SystemVerilog gaps in NVC.

If you're curious, try "hello":

    $ cd hello
    $ ls
    generate_ip.tcl  generate_shells.tcl  Makefile  README.md  tb.vhd
    $ source /opt/xilinx/2026.1/Vivado/settings64.sh
    $ make
    [...eventually...]
    ** Note: 7825ns+0: PASS: 256
       Process :tb:check at tb.vhd:70
    ** Note: 7825ns+0: FINISH called
       Procedure FINISH [] at lib/std.08/env-body.vhd:42
       Process :tb:check at tb.vhd:126

## Why?

* NVC can now (with help) simulate encrypted IP - this has been a longstanding
  and fundamental issue for open-source RTL simulators, which (for obvious and
  fundamental reasons) can't make use of IEEE 1735-style encrypted IP.

* xsim is now a "polyfill" for NVC - a SystemVerilog design that's beyond NVC's
  current capabilities (e.g. AMD/Xilinx xpm) can now instantiate these IP
  blocks in xsi and simulate them as black boxes.

* Similarly, NVC can be a "polyfill" for xsim - when using NVC as a top-level
  simulation kernel and xsim for the "mop-up" terminal IPs, I can make use of
  VHPI, PSL, FST/VCD, cocotb, et cetera.

## Verilator

nvcx can also put Verilog and SystemVerilog cores on
[Verilator](https://www.veripool.org/verilator/), for speed. Cores of either
kind, and NVC's own, share one simulation:

    package require nvcx
    nvcx::write_shell -component fft -output fft_shell.vhd \
        -verilator {-f fft.f -O3}

`-verilator` verilates the module once per generic set the design uses
(`nvcx::verilate`), with nvcx's small harness, into a model cached under
`nvcx.work` like xsim's snapshots. See `man nvcx` for the details.

Verilator's models are 2-state (X and Z inputs read as 0), and Verilator
evaluates a whole time slot at once rather than a delta at a time, so a
Verilator core keeps less of its delta ordering than an xsim core:

* each NVC delta that changes its inputs, and each of its own events, gets one
  evaluation, and every output it changes then changes in the same NVC delta
  (a combinational output derived from a registered one changes with it, not
  a delta later);
* so **a Verilator core must not drive a clock NVC uses**: the clock and data
  registered on it inside the core change in the same NVC delta, and a
  register in NVC clocked by that clock samples the data after its update.
  Generate such clocks in NVC, or in an xsim core.

Clocks into a Verilator core, and its outputs sampled on them, behave as in a
single simulator. `tests/test_verilator.py` pins both concessions down.

nvcx releases carry a Verilator package (`contrib/verilator`): a recent
release, with fixes nvcx needs that are not yet upstream.

## Timing

NVC and each xsim core advance in lockstep. The plugin pauses the xsim kernel
at every delta boundary (a hook on the kernel's signal-update phase, with the
kernel running on a coroutine), writes the inputs NVC changed in its delta,
lets the kernel run one delta, and deposits the outputs that changed for NVC's
next delta. Each side sees the other's assignments one delta after they were
made, as it would in a single simulator. Deltas in which no core output changes
while NVC has nothing pending run without an NVC cycle, since nothing on NVC's
side could have happened in them.  `tests/test_delta.py` checks
delta-cycle properties against NVC simulating the same cores natively.

## Testing

Unfortunately there isn't an obvious public GitHub image containing Vivado
builds, so testing is currently done outside the conventional GitHub CI/CD
infrastructure.

Tested on a Ubuntu image against:

- Vivado 2023.2
- Vivado 2024.1
- Vivado 2025.1
- Vivado 2025.2
- Vivado 2026.1

Older versions (2022.2 and before) can't open a snapshot outside the working
directory through XSI, and nvcx refuses them.

## Install

### From packages

Each [release](https://github.com/gsmecher/nvcx/releases) carries packages for
Debian and Ubuntu. The NVC package replaces NVC's own package of 1.23, which
contains a few patches not yet upstreamed.  For Ubuntu 24.04:

    $ R=https://github.com/gsmecher/nvcx/releases/download/v0.3.0
    $ V=5.054+git202611072109.74cf7aa-1+nvcx2
    $ wget $R/nvc_1.23.0-1+nvcx1_amd64_ubuntu-24.04.deb \
           $R/verilator_${V}_amd64_ubuntu-24.04.deb \
           $R/nvcx_0.3.0-1_amd64_ubuntu-24.04.deb
    $ sudo apt install ./nvc_1.23.0-1+nvcx1_amd64_ubuntu-24.04.deb \
                       ./verilator_${V}_amd64_ubuntu-24.04.deb \
                       ./nvcx_0.3.0-1_amd64_ubuntu-24.04.deb

Substitute `debian-13` or `ubuntu-26.04` as appropriate. The Verilator
package, which nvcx's depends on, is a recent Verilator with fixes nvcx
needs (`contrib/verilator`). Vivado is needed at run time for xsim cores,
located via `XILINX_VIVADO`, and is not packaged.

### From source

You need:

* NVC 1.23, built with `--enable-tcl` and the patches in `contrib/` (these are
  submitted upstream; the `Dockerfile` shows the recipe). nvcx is built
  against NVC's VHPI header, found through the pkg-config file NVC installs
  (`nvc.pc`, from `0005-nvc-pkg-config.patch`),
* CMake 3.24 or later, and pkg-config,
* a C++20 compiler,
* Boost.Context,
* Vivado at run time, for xsim cores, located via `XILINX_VIVADO`, and
* Verilator 5.054 or later at run time, for Verilator cores, with make and a
  C++ compiler (the package from `contrib/verilator` carries fixes nvcx
  needs).

Then:

    $ cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr/local
    $ cmake --build build -j
    $ sudo cmake --install build

### Using it

Either way, the installation includes a VHPI plugin, the bridge entity's VHDL,
a Tcl package, and a pkg-config file, which tells your build where each of
them went:

    $ nvc --work=nvcx:nvcx -a $(pkg-config --variable=vhdl nvcx)
    $ TCLLIBPATH=$(pkg-config --variable=tcldir nvcx) nvc ... --do script.tcl
    $ nvc ... -r --load $(pkg-config --variable=plugin nvcx) top

### With cocotb

cocotb's VHPI library and the nvcx plugin can be loaded into the same NVC run.
NVC 1.23 keeps only the last `--load` option, so give both plugins in one
comma-separated `--load`:

    $ nvc ... --load $COCOTB_VHPI_LIB,$(pkg-config --variable=plugin nvcx) -r top

`$COCOTB_VHPI_LIB` is the path of cocotb's NVC VHPI library
(`cocotb-config --lib-name-path vhpi nvc`). Either order works.

cocotb schedules in femtoseconds, while xsim's default VHDL time resolution is
1 ps. A write that lands between two picoseconds is rejected with "NVC time …
fs is not a multiple of the kernel's … fs tick". Elaborate the xsim snapshot at
1 fs to avoid it, as `bench/neorv32` does:

    $ xelab --timeprecision_vhdl 1fs -dll ...
    # or, with the Tcl flow:
    nvcx::write_shell -component ... -library LIB -elab {-L LIB --timeprecision_vhdl 1fs}

## Build and test

The test suite is Dockerized and is therefore fairly portable:

    $ make
    $ source /opt/xilinx/2026.1/Vivado/settings64.sh
    $ make test
    [...eventually...]
    ..................                                               [100%]
    18 passed in 115.76s (0:01:55)

(Aside: the use of Docker here also allows us to test against multiple Vivado
versions, which are unlikely to all be happy in your host OS image.)

In order to test against all suitable Vivado installations, run:

    $ make test-all  # add VIVADOES="2024.1 2026.1" to specify

## FAQ

**Q: What versions of Vivado are supported?**
> Vivado 2023.2, 2024.1, 2025.1, 2025.2, and 2026.1 are tested via `make
> test-all`. Before 2023, XSI could only open a snapshot from the working
> directory's xsim.dir, so those releases are refused.

**Q: For XPM, should I use `nvcx` or `xpm_vhdl` (`nvc --install xpm_vhdl`)?**
> Your call. `nvcx` will be slower; `xpm_vhdl` has some coverage gaps.  Which
> strategy is better is mostly a question of designer's preference: do you
> trust nvcx's simulator bridge, or xpm_vhdl's faithful reinterpretation of xpm
> RTL?

**Q: How much slower is nvcx than NVC? than xsim?**
> nvcx doesn't add much overhead. The xsim portion of the design runs at xsim
> speeds (slowish); the NVC portion runs at NVC speeds (fast-ish). The result
> is somewhere between the two, weighted by the amount of logic in each domain.
>
> A completely arbitrary benchmark (`make bench`) shows:
>
> ```
> mode    run [s]   uart0.txt
> nvc         5.47   same as nvc
> xsim       10.20   same as nvc
> mixed       8.92   same as nvc
> ```
>
> This includes start-up time; depending on your perspective, this may be totally
> appropriate or may penalize xsim slightly for being slow off the marks.

**Q: Is nvcx affiliated with NVC at all?**
> No! NVC is an excellent project and I should probably have chosen another name.

**Q: Is nvcx a triumph or an abomination?**
> Yes.
