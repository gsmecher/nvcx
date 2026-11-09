// open_core: the backend for a snapshot directory, by what it holds. Kept
// apart from core.cpp, which every backend links, so that only the plugin
// (and whatever else opens snapshots by directory) pulls in all of them.

#include <format>
#include <stdexcept>
#include "nvcx/core.hpp"
#include "nvcx/verilator/model.hpp"
#include "nvcx/xsim/simulation.hpp"

namespace fs = std::filesystem;

namespace nvcx {

std::unique_ptr<Core> open_core(const fs::path& snapshot) {
    // xsim: xelab -dll writes xsimk.so with the snapshot's metadata beside
    // it, in xsim.dir/<snapshot>
    if (fs::is_regular_file(snapshot / "xsimk.so"))
        return std::make_unique<xsim::Simulation>(
            (snapshot / "xsimk.so").string());
    // Verilator: nvcx::verilate builds verilator.so, the model and nvcx's
    // harness for it
    if (fs::is_regular_file(snapshot / "verilator.so"))
        return std::make_unique<verilator::Model>(snapshot / "verilator.so");
    throw std::runtime_error(std::format(
        "snapshot directory {} holds no core nvcx can open (for xsim, an "
        "xsimk.so: elaborate with xelab -dll; for Verilator, a verilator.so: "
        "build it with nvcx::verilate)",
        snapshot.string()));
}

} // namespace nvcx
