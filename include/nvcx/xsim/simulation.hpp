#pragma once

#include <boost/context/fiber.hpp>
#include <cstdint>
#include <string>
#include "nvcx/core.hpp"
#include "nvcx/xsim/kernel.hpp"

namespace nvcx::xsim {

// A core on Vivado's xsim kernel, through XSI. Port numbers are XSI's, and
// sizes are as XSI reports them: bytes for VHDL ports, bits for Verilog ones.
class Simulation final : public Core {
  public:
    // Open the snapshot whose xsimk.so is *design_so* on Kernel::get()
    explicit Simulation(const std::string& design_so);
    ~Simulation() override;
    void close() override;

    uint64_t time() const override {
        make_current();
        return (uint64_t)kernel_->xsi_get_time(handle_);
    }
    int precision() const override {
        make_current();
        return kernel_->xsi_get_int(handle_, xsiTimePrecisionKernel);
    }
    // The kernel runs on a coroutine that yields after each delta's signal
    // updates (see Core::start).
    RunResult start(uint64_t ticks) override;
    RunResult resume() override;
    bool next_event_time(uint64_t& t) const override {
        make_current();
        return kernel_->next_event_time(t);
    }

    void get(const Port& p, uint8_t* buf) const override;
    void put(const Port& p, const uint8_t* buf) override;

    // A message as the kernel formats it ("Failure: ...\nTime: ..." for VHDL,
    // "Fatal: ..." for $fatal): its severity, and its text without the prefix
    static Severity report_severity(const std::string& msg);
    static std::string report_text(const std::string& msg);

  private:
    // Any number of designs share a kernel: this one's state becomes the
    // kernel's globals before anything reads them or runs it.
    void make_current() const;
    std::string last_error();
    void emit_report(const std::string& raw);
    RunResult after_run();
    static void on_active_events(void* eventq);
    // The design's calls to the kernel's $stop, std.env.stop and finish
    // (k_stops in simulation.cpp, entry I), noted in stop_ on the way
    template <int I>
    static void
    on_stop(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
    int stop_ = -1; // the entry of k_stops the design called, if any
    // The kernel runs on a fiber of its own (one per Simulation, kept for
    // its lifetime); main_ is the caller's side while the kernel runs.
    boost::context::fiber run_, main_;
    uint64_t run_ticks_ = 0;
    bool running_ = false; // inside xsi_run on the coroutine
    Kernel* kernel_ = nullptr;
    void* design_handle_ = nullptr;
    XsiHandle handle_ = nullptr;
    XsiSetupInfo setup_{};
    std::string design_dir_, xsim_dir_;
};

} // namespace nvcx::xsim
