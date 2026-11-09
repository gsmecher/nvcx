#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include "nvcx/core.hpp"

struct nvcx_vlt_api;
struct nvcx_vlt_model;

namespace nvcx::verilator {

// A verilated model, built by nvcx.tcl (nvcx::verilate) with its harness
// into verilator.so (src/verilator/shim). Each Model is an instance with a
// VerilatedContext of its own; instances of one model share its library.
//
// Verilator evaluates a time slot to convergence in one call: a Model never
// yields between delta cycles. start() brings it to the target time, through
// its own events, and evaluates there if anything is due or an input was
// written since; with the bridge, each NVC delta that changes its inputs gets
// one such evaluation, and its outputs settle in NVC's next delta. Within a
// time slot, the core's own delta ordering is therefore lost: see the README
// (a Verilator core must not drive a clock NVC uses).
//
// Models are 2-state: an input bit that is X or Z (U, W, - ...) reads as 0,
// and outputs are only ever 0 or 1.
class Model final : public Core {
  public:
    explicit Model(const std::filesystem::path& so);
    ~Model() override;

    void close() override;
    uint64_t time() const override;
    int precision() const override;
    RunResult start(uint64_t ticks) override;
    RunResult resume() override;
    bool next_event_time(uint64_t& t) const override;
    void get(const Port& p, uint8_t* buf) const override;
    void put(const Port& p, const uint8_t* buf) override;

    // A line of the model's output, as Verilator prints it: a report
    // ("[T] %Warning: file:line: hier: text", or the runtime's "%Error:
    // file:line: text") as its severity and text, or else plain output
    struct Line {
        bool report = false;
        Severity severity = Severity::Note;
        std::string text;
    };
    static Line parse_line(const std::string& line);

  private:
    static void on_print(void* user, const char* line);
    void emit_lines();
    RunResult eval();

    void* library_ = nullptr;
    const nvcx_vlt_api* api_ = nullptr;
    nvcx_vlt_model* model_ = nullptr;
    // an input was written, or the model was never evaluated: due an eval
    bool dirty_ = true;
    bool failed_ = false;            // the runtime's fatal error
    std::vector<std::string> lines_; // printed in the call into the model
    mutable std::vector<uint32_t> words_;
};

} // namespace nvcx::verilator
