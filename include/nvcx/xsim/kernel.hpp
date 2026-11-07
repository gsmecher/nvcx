#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace nvcx::xsim {

struct XsiSetupInfo {
    char* logFileName;
    char* wdbFileName;
    char* xsimDir;
};

using XsiHandle = void*;

enum : int {
    xsiNumTopPorts = 1,
    xsiTimePrecisionKernel = 2,
    xsiDirectionTopPort = 3,
    xsiHDLValueSize = 4,
    xsiNameTopPort = 5,
};

enum : int {
    xsiInputPort = 1,
    xsiOutputPort = 2,
    xsiInoutPort = 3,
};

enum : int {
    xsiError = 2,
    xsiFatalError = 3,
};

class Kernel {
  public:
    // Vivado's kernel ($XILINX_VIVADO), loaded on first use: one per
    // process, shared by every design
    static Kernel& get();
    const std::string& path() const {
        return path_;
    }

    // XSI public C API (xsi_open lives in the design library)
    void (*xsi_close)(XsiHandle) = nullptr;
    void (*xsi_run)(XsiHandle, uint64_t) = nullptr;
    void (*xsi_put_value)(XsiHandle, int, void*) = nullptr;
    int (*xsi_get_value)(XsiHandle, int, void*) = nullptr;
    int (*xsi_get_status)(XsiHandle) = nullptr;
    const char* (*xsi_get_error_info)(XsiHandle) = nullptr;
    int (*xsi_get_int)(XsiHandle, int) = nullptr;
    int (*xsi_get_int_port)(XsiHandle, int, int) = nullptr;
    const char* (*xsi_get_str_port)(XsiHandle, int, int) = nullptr;
    int64_t (*xsi_get_time)(XsiHandle) = nullptr;

    // ISIMK::XSIHost: whether a top-level port carries VHDL values (one byte
    // per scalar) or Verilog ones ({aVal, bVal} words); XSI itself does not
    // say, and reports sizes in bytes for one and bits for the other.
    bool (*host_isPortVHDL)(void*, int) = nullptr;

    // ISIMK::Host::setGlobals(bool): installs a design's state as the
    // kernel's process-wide globals (true) or clears them (false). Every
    // xsi_* entry point but xsi_get_time starts with it, which is how one
    // kernel serves several open designs.
    void (*host_setGlobals)(XsiHandle, bool) = nullptr;
    // The design whose state the globals hold, if known
    XsiHandle current = nullptr;

    // Exported globals (pointers to the kernel's storage)
    void** globalDesignProperties = nullptr;
    void** globalEventQueue = nullptr;

    // ISIMK::EventQ::processActiveEvents, the signal-update phase that opens
    // every delta cycle. The scheduler calls it through the kernel's own PLT,
    // so a function written into its GOT slot sees every delta boundary; the
    // hook must call process_active_events itself.
    void (*process_active_events)(void*) = nullptr;
    void hook_active_events(void (*hook)(void*));

    // The kernel's release, "2026.1", from ISIM::VersionUtils::populateVersion
    // (2025.2 and later, else "".)
    std::string version;

    // GlobalDesignProperties -> e_kernel_status_kind, found by reading the
    // kernel's own code for it (XSI has no "finished" status).
    size_t status_offset = 0;
    int status_kind() const {
        return status_offset && *globalDesignProperties
                   ? *reinterpret_cast<int32_t*>(
                         static_cast<char*>(*globalDesignProperties) +
                         status_offset)
                   : -1;
    }

    // Time of the earliest scheduled kernel event, or false if the event
    // queue is empty.
    bool next_event_time(uint64_t& t) const;

  private:
    explicit Kernel(const std::string& path);
    void resolve_all();
    void discover_offsets();
    std::string path_;
    void* handle_ = nullptr;
};

} // namespace nvcx::xsim

// Numeric constants of the kernel's interface, found empirically. Keep
// them here so that a Vivado release that changes one is a one-file fix.
namespace nvcx::xsim::abi {

// kernel globals: GlobalDesignProperties' status kind, the values that end a
// simulation (2 is "stopped", which discover_offsets looks for)
enum KernelStatus : int32_t {
    KS_FINISHED = 8,
    KS_VHDL_FINISH = 10,
    KS_CLOSED = 12,
};

// event queue: GlobalEventQueue -> HdlQ list head, time slot layout
constexpr size_t LISTQ_HEAD = 0xb0;
constexpr size_t TIMESLOT_TIME = 0x0;
constexpr size_t TIMESLOT_NEXT = 0x10;

} // namespace nvcx::xsim::abi
