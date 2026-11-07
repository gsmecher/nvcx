#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nvcx {

// A core: a design simulated by some other simulator (a backend) behind a
// shell entity, seen by the bridge only through its top-level ports and its
// progress in time. Each backend implements this; open_core picks one.
//
// Time advances in steps the bridge interleaves with NVC's delta cycles. A
// backend that can pause between delta cycles (xsim) yields at each one; one
// that cannot (it runs a time step to convergence) never yields, and each
// start() of zero ticks settles the inputs just written.

enum class Direction {
    None,
    In,
    Out,
    Inout,
};

// Verilog values cross as {aVal, bVal} word pairs, one per 32 bits, bit 0 of
// word 0 the least significant: (0,0) is 0, (1,0) 1, (0,1) Z and (1,1) X.
struct VlogLogicVal {
    uint32_t aVal, bVal;
};

class Core {
  public:
    // A top-level port. Values cross in one of two layouts: VHDL one byte per
    // scalar (its enumeration position; 4-byte integers, 8-byte reals), or
    // Verilog's VlogLogicVal words.
    struct Port {
        int index = 0; // the backend's port number
        std::string name;
        Direction dir = Direction::None;
        bool vhdl = true;  // value layout
        unsigned size = 0; // bytes (VHDL) or bits (Verilog)
        size_t raw_size() const {
            return vhdl ? size
                        : ((size_t)size + 31) / 32 * sizeof(VlogLogicVal);
        }
    };
    enum class RunStatus {
        Completed, // quiescent at the target time
        Finished,  // the design ended its own simulation
        Error,
        Yielded, // paused at a delta boundary (see start)
    };
    struct RunResult {
        RunStatus status;
        std::string message;
    };
    enum class Severity : int {
        Note,    // and $info
        Warning, // and $warning
        Error,   // and $error
        Failure, // and $fatal
    };
    // A report or assertion from the core: its severity, and its text without
    // the backend's severity prefix or location.
    using ReportSink = std::function<void(Severity, const std::string&)>;

    Core() = default;
    virtual ~Core() = default;
    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;

    // Finish with the design (also done by the destructor).
    virtual void close() = 0;

    const std::vector<Port>& ports() const {
        return ports_;
    }
    const Port* port(const std::string& name) const;

    // Time in units of 10^precision() seconds.
    virtual uint64_t time() const = 0;
    virtual int precision() const = 0;

    // Run *ticks* time units (zero: settle the current time) to quiescence.
    RunResult run(uint64_t ticks);
    // The same, a step at a time. A backend that pauses at delta boundaries
    // returns Yielded from start() and each resume() after a delta's signal
    // updates, with that delta's processes still to run: the caller can read
    // the updated outputs and write inputs the processes will see. The other
    // statuses mean the backend returned and is quiescent.
    virtual RunResult start(uint64_t ticks) = 0;
    virtual RunResult resume() = 0;
    bool suspended() const {
        return suspended_;
    }
    bool finished() const {
        return finished_;
    }
    // Earliest scheduled event, in ticks, or false if there is none.
    virtual bool next_event_time(uint64_t& t) const = 0;

    // Port values, Port::raw_size() bytes in the port's layout.
    virtual void get(const Port& p, uint8_t* buf) const = 0;
    virtual void put(const Port& p, const uint8_t* buf) = 0;

    // Where reports go; by default, to stdout as the backend formats them.
    void set_report_sink(ReportSink sink) {
        report_sink_ = std::move(sink);
    }

  protected:
    // A backend's report: *formatted* is what the default sink prints.
    void
    report(Severity s, const std::string& text, const std::string& formatted);

    std::vector<Port> ports_;
    bool suspended_ = false; // paused at a delta boundary
    bool finished_ = false;

  private:
    ReportSink report_sink_;
};

// Open the core a snapshot directory holds, on the backend that built it:
// xsim's xsimk.so (xelab -dll). Throws std::runtime_error if there is none, or
// if it cannot be opened.
std::unique_ptr<Core> open_core(const std::filesystem::path& snapshot);

// Verilog values in VlogLogicVal layout, as logic strings (MSB first, as for
// VHDL).
std::string vlog_to_logic(const uint8_t* raw, unsigned nbits);
void logic_to_vlog(const std::string& s,
                   uint8_t* raw,
                   unsigned nbits); // 'L'->0, 'H'->1, Z->Z, else X
// std_ulogic by enumeration position, which is also the VHDL byte layout.
inline constexpr char LOGIC_CHARS[] = "UX01ZWLH-";

} // namespace nvcx
