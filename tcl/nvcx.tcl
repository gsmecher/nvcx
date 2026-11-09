# nvcx: NVC's side of the Tcl flow.
#
# At this point in the flow, NVC has parsed and elaborated the overall design
# enough that we can walk it and identify cores needing to connect to another
# simulator. When we find these cores,
#
# - emit a VHDL shell containing a bridge component for NVC
# - build the underlying component's model: an xsim snapshot (xelab), or a
#   verilated model (verilator)
#
# ...which allows the gap between NVC and the other simulator to be closed.
# After that, the generated VHDL shells need to be elaborated by NVC and the
# simulator can start.

package require sha256
package require fileutil
package require cmdline

namespace eval nvcx {
    # Component instances, collected on first use
    variable found {}

    # This package's directory, which holds the Verilator shim's sources too
    variable here [file dirname [file normalize [info script]]]

    # Snapshot cache
    proc workdir {} {
        if {[info exists ::env(NVCX_WORK)] && $::env(NVCX_WORK) ne ""} {
            return $::env(NVCX_WORK)
        }
        return nvcx.work
    }

    # "2026.1" from $XILINX_VIVADO's path, else "unknown"
    proc vivado_version {} {
        if {[info exists ::env(XILINX_VIVADO)]
            && [regexp {\d{4}\.\d+} $::env(XILINX_VIVADO) v]} {
            return $v
        }
        return unknown
    }

    # Rename directory TMP to DIR, which must not exist (or be empty): false,
    # with TMP deleted, if it does. rename(2), through mv -T: Tcl's file
    # rename would move TMP into an existing DIR instead of failing.
    proc publish {tmp dir} {
        if {[catch {exec mv -T -- $tmp $dir}]} {
            file delete -force $tmp
            return 0
        }
        return 1
    }

    # The snapshot of LIBRARY.ENTITY elaborated with ELAB (xelab arguments)
    # and the generics TOPS (a list of -generic_top values): its
    # directory xsim.dir/ENTITY in the cache, running xelab on a miss. A
    # finished temporary directory is renamed into place, so a half-written
    # snapshot is never seen and concurrent scripts cannot clash: whoever
    # renames first wins and the other uses that.
    proc elaborate {library entity elab tops} {
        set snapshot "$library.$entity"
        set key "$snapshot|$elab|[join $tops "\n"]|[vivado_version]"
        set snapshots [file join [workdir] snapshots]
        set dir [file join $snapshots "$entity-[::sha2::sha256 -hex -- $key]"]
        set so [file join $dir xsim.dir $entity xsimk.so]
        if {[file exists $so]} { return [file normalize [file dirname $so]] }
        file mkdir $snapshots
        set tmp [::fileutil::maketempdir -prefix "[file tail $dir]." -dir $snapshots]
        ::fileutil::writeFile [file join $tmp key.txt] $key
        set args [list -dll {*}$elab]
        foreach g $tops { lappend args -generic_top $g }
        lappend args --snapshot $entity $snapshot
        set xelab xelab
        if {[info exists ::env(XILINX_VIVADO)]} {
            set xelab [file join $::env(XILINX_VIVADO) bin xelab]
        }
        set log [file normalize [file join $tmp xelab.log]]
        set cwd [pwd]
        cd $tmp
        set failed [catch {exec $xelab {*}$args >& xelab.log} msg]
        cd $cwd
        if {$failed} {
            error "xelab failed for $snapshot ($msg); see $log"
        }
        if {![file exists [file join $tmp xsim.dir $entity xsimk.so]]} {
            error "xelab wrote no snapshot for $snapshot; see $log"
        }
        if {![publish $tmp $dir] && ![file exists $so]} {
            error "cannot publish $dir"
        }
        return [file normalize [file dirname $so]]
    }

    # "Verilator 5.054 2026-11-01 rev ..." from the verilator on PATH
    proc verilator_version {} {
        variable verilator_version
        if {![info exists verilator_version]} {
            if {[catch {exec verilator --version} verilator_version]} {
                error "cannot run verilator: $verilator_version"
            }
        }
        return $verilator_version
    }

    # nvcx_ports.h, for the shim: an NVCX_PORT(name, dir, width, member) per
    # top-level port of the model Vcore.h declares (VL_IN8(&clk,0,0); ...)
    proc verilator_ports {header out} {
        set ports {}
        # Vcore.h lists the ports, and nothing else, under "// PORTS", up to a
        # blank line. One this cannot describe (an unpacked array or a real,
        # which are references there, not VL_IN and friends) must not go
        # missing quietly: the bridge would leave the shell's port undriven.
        set section 0
        foreach line [split [::fileutil::cat $header] "\n"] {
            if {[regexp {^\s*// PORTS\s*$} $line]} { set section 1; continue }
            if {[string trim $line] eq ""} { set section 0 }
            if {![regexp {^\s*VL_(IN|OUT|INOUT)(\w*)\(&(\w+),\s*(-?\d+),\s*(-?\d+)} $line -> dir type member msb lsb]} {
                if {$section && ![regexp {^\s*//} $line]} {
                    error "port [string trim $line]: only integral ports are supported (not unpacked arrays or reals)"
                }
                continue
            }
            if {$type ni {"" 8 16 64 W}} {
                error "port $member: Verilator type VL_$dir$type is not supported (only integral ports are)"
            }
            # Verilator spells a character a C++ identifier cannot hold as
            # __0HH; the Verilog name has the character itself
            set name [subst -nocommands -novariables [regsub -all {__0([0-9a-fA-F]{2})} $member {\\x\1}]]
            set width [expr {abs($msb - $lsb) + 1}]
            lappend ports "NVCX_PORT(\"[string map {\\ \\\\ \" \\\"} $name]\", NVCX_VLT_$dir, $width, $member)"
        }
        ::fileutil::writeFile $out "[join $ports \n]\n"
    }

    # nvcx_threads.h, for the shim: the threads the model Vcore.cpp defines
    # was verilated for (--threads; 1 without), which its VerilatedContext
    # must have before the model is constructed
    proc verilator_threads {source out} {
        if {![regexp {unsigned Vcore::threads\(\) const \{ return (\d+); \}} [::fileutil::cat $source] -> n]} {
            error "cannot find the model's thread count in $source"
        }
        ::fileutil::writeFile $out "#define NVCX_VLT_THREADS $n\n"
    }

    # A digest of nvcx's harness, which every model is built with: a model
    # built with another harness is stale
    proc verilator_harness {} {
        variable here
        set digests {}
        foreach f [lsort [glob -directory $here nvcx_verilator_*]] {
            lappend digests [::sha2::sha256 -hex -filename $f]
        }
        return [::sha2::sha256 -hex -- [join $digests]]
    }

    # The sources a model in *dir* was verilated from, with their contents'
    # digests: what Verilator read (its Vcore__verFiles.dat), so -y
    # libraries, `include files and -f files too. Throws if one is missing.
    proc verilator_digest {paths} {
        if {![llength $paths]} { return "" }
        return [exec sha256sum -- {*}$paths]
    }
    proc verilator_sources {obj} {
        set paths {}
        foreach line [split [::fileutil::cat [file join $obj Vcore__verFiles.dat]] "\n"] {
            # S <size> <ino> <ctime> <ns> <mtime> <ns> "<hash>" "<path>"
            if {[regexp {^S\s.*"([^"]*)"\s+"(.*)"$} $line -> hash path] && $hash ne "unhashed"} {
                lappend paths $path
            }
        }
        return $paths
    }
    # Whether the model in *dir* is still that of its sources
    proc verilator_fresh {dir} {
        set record [file join $dir sources.txt]
        if {![file exists $record]} { return 0 }
        lassign [::fileutil::cat $record] paths digest
        return [expr {![catch {verilator_digest $paths} now] && $now eq $digest}]
    }

    # nvcx::verilate TOP ARGS ?PARAMS?
    #
    # The model of Verilog module TOP verilated with ARGS (verilator's: the
    # sources, -I, -f, and any other options) and the parameters PARAMS (a
    # list of NAME=VALUE, as for -G), built with nvcx's harness into a
    # snapshot directory holding verilator.so, for nvcx::write_shell
    # -snapshot or the bridge. Cached like elaborate's snapshots, keyed by
    # the arguments, Verilator's version and nvcx's harness, and rebuilt when
    # the contents of any source it was verilated from change.
    #
    # Always --timing: nvcx needs the model's own events (#delays) to visit
    # their times. Lint warnings are not fatal (-Wno-fatal), since cores are
    # often third-party RTL.
    proc verilate {top vargs {params {}}} {
        variable here
        set key "verilator|$top|$vargs|[join $params "\n"]|[verilator_version]|[package present nvcx]|[verilator_harness]"
        set snapshots [file join [workdir] snapshots]
        set dir [file join $snapshots "$top-vl-[::sha2::sha256 -hex -- $key]"]
        set so [file join $dir verilator.so]
        if {[file exists $so] && [verilator_fresh $dir]} { return [file normalize $dir] }
        file mkdir $snapshots
        set tmp [::fileutil::maketempdir -prefix "[file tail $dir]." -dir $snapshots]
        ::fileutil::writeFile [file join $tmp key.txt] $key
        set obj [file join $tmp obj]
        set log [file normalize [file join $tmp verilator.log]]
        set args [list --cc --timing --top-module $top --prefix Vcore -Mdir $obj -Wno-fatal \
            --exe [file join $here nvcx_verilator_shim.cpp] -o verilator.so \
            -CFLAGS -fPIC -CFLAGS -I$here \
            -LDFLAGS -shared -LDFLAGS -Wl,--version-script=[file join $here nvcx_verilator_shim.map]]
        foreach p $params { lappend args -G$p }
        lappend args {*}$vargs
        if {[catch {exec verilator {*}$args >& $log} msg]} {
            error "verilator failed for $top ($msg); see $log"
        }
        verilator_ports [file join $obj Vcore.h] [file join $obj nvcx_ports.h]
        verilator_threads [file join $obj Vcore.cpp] [file join $obj nvcx_threads.h]
        set jobs 1
        catch { set jobs [exec getconf _NPROCESSORS_ONLN] }
        # The hooks go only into Verilator's runtime (OPT_GLOBAL is its
        # objects' alone; the harness includes them itself): the model's own
        # objects use a precompiled header when Verilator splits a large
        # model, and GCC uses one only if it is the first header included.
        set hooks "OPT_GLOBAL=-Os -include [file join $here nvcx_verilator_hooks.h]"
        if {[catch {exec make -C $obj -f Vcore.mk -j$jobs $hooks >>& $log} msg]} {
            error "building the model of $top failed ($msg); see $log"
        }
        file rename [file join $obj verilator.so] [file join $tmp verilator.so]
        set paths [verilator_sources $obj]
        ::fileutil::writeFile [file join $tmp sources.txt] [list $paths [verilator_digest $paths]]
        if {[file exists $dir] && ![verilator_fresh $dir]} {
            # stale: its sources changed. Set aside rather than deleted, in
            # case a simulation has it open
            catch { exec mv -T -- $dir "$dir.stale.[file tail $tmp]" }
        }
        # (lost to a concurrent script: its model must be of these sources)
        if {![publish $tmp $dir] && !([file exists $so] && [verilator_fresh $dir])} {
            error "cannot publish $dir"
        }
        return [file normalize $dir]
    }

    # VHDL for the string TEXT, one literal per line joined with LF.
    proc vhdl_lines {text} {
        if {$text eq ""} { return {""} }
        set parts {}
        foreach line [split $text "\n"] {
            lappend parts "\"[string map {\" \"\"} $line]\""
        }
        return [join $parts " & LF & "]
    }

    # VHPI base type name -> the VHDL spelling a shell may use for a port
    # (arrays unconstrained: the width comes from the actual) and for a
    # generic.
    proc port_type {base} {
        switch -- $base {
            STD_ULOGIC         { return std_ulogic }
            STD_ULOGIC_VECTOR  { return std_ulogic_vector }
            UNRESOLVED_UNSIGNED { return unsigned }
            UNRESOLVED_SIGNED  { return signed }
            UNSIGNED           { return unsigned }
            SIGNED             { return signed }
            BIT                { return bit }
            BIT_VECTOR         { return bit_vector }
            INTEGER            { return integer }
            REAL               { return real }
            BOOLEAN            { return boolean }
        }
        error "port type $base is not supported"
    }
    proc generic_type {base} {
        switch -- $base {
            INTEGER { return integer }
            STRING  { return string }
            REAL    { return real }
            BOOLEAN { return boolean }
        }
        error "generic type $base is not supported"
    }
    # A generic's value as VHDL source, and as the text the shell's GENERICS
    # expression yields for it at run time (what the snapshot table matches).
    proc generic_literal {type value} {
        switch -- $type {
            string  { return "\"$value\"" }
            boolean { return [string tolower $value] }
            real    { return [generic_text real $value] }
            default { return $value }
        }
    }
    proc generic_text {type value} {
        switch -- $type {
            boolean { return [string tolower $value] }
            real    {
                # NVC's real'image: the shortest round-trip form, no ".0"
                set v [expr {double($value)}]
                return [regsub {\.0$} $v ""]
            }
            default { return $value }
        }
    }
    proc generic_image {type name} {
        switch -- $type {
            string  { return $name }
            default { return "${type}'image($name)" }
        }
    }

    # A port's name as the shell declares it: a basic identifier in lower
    # case, an extended one (\next\, for a Verilog port named after a VHDL
    # reserved word) verbatim, since its case is part of it
    proc port_name {name} {
        if {[string index $name 0] eq "\\"} { return $name }
        return [string tolower $name]
    }

    # Every component instance under *h*, bound or not: component name ->
    # list of {generics ports fullname}, one per instance, where generics is
    # a list of {name type value} and ports a list of {name mode type}.
    proc collect {h found_var} {
        upvar 1 $found_var found
        # only a component instance has a component name (contrib patch)
        set comp ""
        catch { set comp [string tolower [vhpi::get_str CompName $h]] }
        if {$comp ne ""} {
            # A type or mode a shell cannot express is recorded as
            # {unsupported WHY}, and is an error only if a shell is asked
            # for this component: a bound instance (a UNISIM primitive, say)
            # may use whatever it likes.
            set generics {}
            foreach g [vhpi::iterate GenericDecls $h] {
                if {[catch {
                    set type [generic_type [vhpi::get_str Name [vhpi::handle BaseType $g]]]
                    set value [vhpi::get_value $g]
                } why]} {
                    set type [list unsupported $why]
                    set value ""
                }
                # the name as declared: a Verilog parameter is case-sensitive for -generic_top
                lappend generics [list [vhpi::get_str Name $g] $type $value]
            }
            set ports {}
            foreach p [vhpi::iterate PortDecls $h] {
                set mode [vhpi::get Mode $p]
                if {$mode == 1001} { set mode in } elseif {$mode == 1002} { set mode out } else {
                    set mode [list unsupported "only in and out ports are supported"]
                }
                if {[catch {port_type [vhpi::get_str Name [vhpi::handle BaseType $p]]} type]} {
                    set type [list unsupported $type]
                }
                lappend ports [list [port_name [vhpi::get_str Name $p]] $mode $type]
            }
            dict lappend found $comp [list $generics $ports [vhpi::get_str FullName $h]]
        }
        foreach r [vhpi::iterate InternalRegions $h] { collect $r found }
    }

    # Write the shell for component *name* to channel *chan*: *what* is
    # {snapshot DIR}, {library LIB ELAB TABLE} or {verilator ARGS TABLE},
    # with TABLE a dict generics text -> snapshot directory; *instances* the
    # list collect built.
    proc emit_shell {chan name what instances} {
        lassign [lindex $instances 0] generics ports
        lassign $what kind arg elab table
        if {$kind eq "verilator"} {
            set table $elab
        }
        puts $chan "-- Generated by nvcx.tcl for component $name, from NVC's view of its declaration."
        switch -- $kind {
            snapshot {
                set backend [expr {[file exists [file join $arg verilator.so]] ? "verilator" : "xsim"}]
                puts $chan "-- Simulates the $backend snapshot $arg. Its generics were fixed when it"
                puts $chan "-- was built: the instantiation's values are the defaults here."
            }
            library {
                set backend xsim
                puts $chan "-- Generics are chosen at the instantiation point; nvcx.tcl elaborated an"
                puts $chan "-- xsim snapshot for each set the design uses (xelab $elab)."
            }
            verilator {
                set backend verilator
                puts $chan "-- Generics are chosen at the instantiation point; nvcx.tcl verilated a"
                puts $chan "-- model for each set the design uses (verilator $arg)."
            }
        }
        puts $chan "library ieee;"
        puts $chan "use ieee.std_logic_1164.all;"
        puts $chan "use ieee.numeric_std.all;"
        puts $chan "library nvcx;"
        puts $chan ""
        puts $chan "entity $name is"
        if {[llength $generics]} {
            set lines {}
            foreach g $generics {
                lassign $g gname gtype gvalue
                lappend lines "        $gname : $gtype := [generic_literal $gtype $gvalue]"
            }
            puts $chan "    generic ("
            puts $chan [join $lines ";\n"]
            puts $chan "    );"
        }
        set lines {}
        foreach p $ports {
            lassign $p pname pmode ptype
            lappend lines "        $pname : $pmode $ptype"
        }
        puts $chan "    port ("
        puts $chan [join $lines ";\n"]
        puts $chan "    );"
        puts $chan "end entity;"
        puts $chan ""
        puts $chan "architecture $backend of $name is"
        if {$kind eq "snapshot"} {
            puts $chan "begin"
            puts $chan "    core : entity nvcx.bridge generic map (SNAPSHOT => \"[file normalize $arg]\");"
        } else {
            # the instantiation's generics as the NAME=VALUE lines the
            # snapshot table is keyed by
            set parts {}
            foreach g $generics {
                lassign $g gname gtype
                lappend parts "\"$gname=\" & [generic_image $gtype $gname]"
            }
            if {![llength $parts]} { set parts {{""}} }
            puts $chan "    constant GENERICS : string :="
            puts $chan "        [join $parts "\n        & LF & "];"
            puts $chan "    -- the snapshot elaborated for this instantiation's generics (\"\" if none)"
            puts $chan "    function nvcx_snapshot return string is"
            puts $chan "    begin"
            dict for {text dir} $table {
                puts $chan "        if GENERICS = [vhdl_lines $text] then"
                puts $chan "            return \"$dir\";"
                puts $chan "        end if;"
            }
            puts $chan "        return \"\";"
            puts $chan "    end function;"
            puts $chan "begin"
            puts $chan "    core : entity nvcx.bridge"
            puts $chan "        generic map (SNAPSHOT => nvcx_snapshot, GENERICS => GENERICS);"
        }
        puts $chan "end architecture;"
    }

    # An instance's generics as -generic_top values for xelab. A string is
    # quoted: unquoted, xelab takes a numeric-looking one ("0") as an integer.
    proc generic_tops {generics} {
        set tops {}
        foreach g $generics {
            lassign $g gname gtype gvalue
            set v [generic_text $gtype $gvalue]
            if {$gtype eq "string"} { set v "\"[string map {\" \\\"} $v]\"" }
            lappend tops "$gname=$v"
        }
        return $tops
    }

    # An instance's generics as Verilator -G values (NAME=VALUE). A string
    # is quoted, as Verilator expects; a boolean is 1 or 0.
    proc generic_params {generics} {
        set params {}
        foreach g $generics {
            lassign $g gname gtype gvalue
            set v [generic_text $gtype $gvalue]
            switch -- $gtype {
                string  { set v "\"[string map {\" \\\" \\ \\\\} $v]\"" }
                boolean { set v [expr {$v ? 1 : 0}] }
            }
            lappend params "$gname=$v"
        }
        return $params
    }

    # The GENERICS text an instance's values produce at run time.
    proc generics_text {generics} {
        set parts {}
        foreach g $generics {
            lassign $g gname gtype gvalue
            lappend parts "$gname=[generic_text $gtype $gvalue]"
        }
        return [join $parts "\n"]
    }

    # The component instances of the elaborated design, by component name
    # (lower case): name -> list of {generics ports fullname}.
    proc instances {} {
        variable found
        if {![dict size $found]} {
            set found [dict create]
            collect [vhpi::handle RootInst 0] found
        }
        return $found
    }

    # nvcx::write_shell -component NAME -output FILE
    #                   (-snapshot DIR | -library LIB ?-elab ARGS? | -verilator ARGS)
    #
    # Writes the VHDL shell for the component NAME to FILE and, for -library,
    # elaborates the xsim snapshot LIB.NAME for every generic set the design
    # instantiates it with (ARGS to xelab; default "-L LIB"); for -verilator,
    # verilates Verilog module NAME for every generic set (ARGS to verilator:
    # the sources and any options; see verilate). -snapshot names a snapshot
    # built already (xelab -dll's xsim.dir/NAME, or nvcx::verilate's
    # directory), whose generics the instantiation must leave at their values.
    proc write_shell {args} {
        set options {
            {component.arg "" "the component, as declared in the design"}
            {output.arg    "" "the VHDL shell to write"}
            {snapshot.arg  "" "a snapshot built already (xsim.dir/<name>, or nvcx::verilate's)"}
            {library.arg   "" "or: elaborate LIBRARY.<component> for each generic set (xsim)"}
            {elab.arg      "" "xelab arguments with -library (default: -L LIBRARY)"}
            {verilator.arg "" "or: verilate <component> for each generic set, with these verilator arguments"}
        }
        set usage "-component NAME -output FILE (-snapshot DIR | -library LIB ?-elab ARGS? | -verilator ARGS)"
        if {[catch {::cmdline::getoptions args $options $usage} result]} {
            error "nvcx::write_shell: $result"
        }
        array set opt $result
        if {[llength $args]} {
            error "nvcx::write_shell: unexpected arguments: $args"
        }
        set name [string tolower $opt(component)]
        set path $opt(output)
        set snapshot $opt(snapshot)
        set library $opt(library)
        set elab $opt(elab)
        set verilator $opt(verilator)
        if {$name eq "" || $path eq ""} {
            error "nvcx::write_shell: wants -component NAME and -output FILE"
        }
        if {($snapshot ne "") + ($library ne "") + ($verilator ne "") != 1} {
            error "nvcx::write_shell $name: wants exactly one of -snapshot DIR, -library LIB or -verilator ARGS"
        }
        if {$snapshot ne "" && $elab ne ""} {
            error "nvcx::write_shell $name: -elab only applies with -library"
        }
        set all [instances]
        if {![dict exists $all $name]} {
            error "nvcx::write_shell $name: the design instantiates no component of that name (components: [join [dict keys $all] ", "])"
        }
        set instances [dict get $all $name]
        foreach inst $instances {
            lassign $inst generics ports full
            foreach g $generics {
                lassign $g gname gtype
                if {[lindex $gtype 0] eq "unsupported"} {
                    error "nvcx::write_shell $name: $full: generic $gname: [lindex $gtype 1]"
                }
            }
            foreach p $ports {
                lassign $p pname pmode ptype
                foreach what [list $pmode $ptype] {
                    if {[lindex $what 0] eq "unsupported"} {
                        error "nvcx::write_shell $name: $full: port $pname: [lindex $what 1]"
                    }
                }
            }
        }
        if {$snapshot ne ""} {
            set what [list snapshot $snapshot]
        } elseif {$verilator ne ""} {
            # one model per distinct generic set, cached across runs. The
            # component's name is the module's: Verilog's case, as the
            # design declares it, matters
            set table [dict create]
            foreach inst $instances {
                lassign $inst generics ports full
                set text [generics_text $generics]
                if {[dict exists $table $text]} continue
                puts "nvcx: verilating $opt(component) for $full"
                set dir [verilate $opt(component) $verilator [generic_params $generics]]
                puts "nvcx:   $dir"
                dict set table $text $dir
            }
            set what [list verilator $verilator $table]
        } else {
            if {$elab eq ""} { set elab "-L $library" }
            # one snapshot per distinct generic set, cached across runs
            set table [dict create]
            foreach inst $instances {
                lassign $inst generics ports full
                set text [generics_text $generics]
                if {[dict exists $table $text]} continue
                puts "nvcx: elaborating $library.$name for $full"
                set dir [elaborate $library $name $elab [generic_tops $generics]]
                puts "nvcx:   $dir"
                dict set table $text $dir
            }
            set what [list library $library $elab $table]
        }
        set chan [open $path w]
        emit_shell $chan $name $what $instances
        close $chan
        puts "nvcx: $path ([llength $instances] instance[expr {[llength $instances] == 1 ? "" : "s"}])"
        return $path
    }
}
