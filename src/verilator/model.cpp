#include "nvcx/verilator/model.hpp"

#include <dlfcn.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <format>
#include <stdexcept>
#include "nvcx_verilator_abi.h"

namespace nvcx::verilator {

namespace {

    // "file.v:12: " at the start of *s*: its length, or 0
    size_t location_length(const std::string& s, size_t pos) {
        size_t colon = s.find(':', pos);
        if (colon == std::string::npos || colon == pos)
            return 0;
        size_t i = colon + 1;
        if (i >= s.size() || !isdigit((unsigned char)s[i]))
            return 0;
        while (i < s.size() && isdigit((unsigned char)s[i]))
            i++;
        if (s.compare(i, 2, ": ") != 0)
            return 0;
        return i + 2 - pos;
    }

} // namespace

Model::Model(const std::filesystem::path& so) {
    dlerror();
    library_ = dlopen(so.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library_)
        throw std::runtime_error(
            std::format("cannot load model {}: {}", so.string(), dlerror()));
    try {
        auto entry = reinterpret_cast<const nvcx_vlt_api* (*)()>(
            dlsym(library_, "nvcx_verilator_api"));
        if (!entry)
            throw std::runtime_error(std::format(
                "{} is not a model nvcx built (no nvcx_verilator_api)",
                so.string()));
        api_ = entry();
        if (api_->abi_version != NVCX_VLT_ABI_VERSION)
            throw std::runtime_error(
                std::format("model {} was built for another nvcx (harness "
                            "version {}, this nvcx's {}): rebuild it",
                            so.string(),
                            api_->abi_version,
                            NVCX_VLT_ABI_VERSION));
        for (unsigned i = 0; i < api_->nports; i++) {
            const nvcx_vlt_port& vp = api_->ports[i];
            Port p;
            p.index = (int)i;
            p.name = vp.name;
            p.dir = vp.dir == NVCX_VLT_IN    ? Direction::In
                    : vp.dir == NVCX_VLT_OUT ? Direction::Out
                                             : Direction::Inout;
            p.vhdl = false;
            p.size = vp.width;
            ports_.push_back(std::move(p));
        }
        const nvcx_vlt_sink sink{.user = this, .print = on_print};
        model_ = api_->open(&sink);
        if (!model_)
            throw std::runtime_error("cannot instantiate model " + so.string());
    } catch (...) {
        dlclose(library_);
        throw;
    }
}

void Model::close() {
    if (model_) {
        api_->close(model_); // its final blocks
        model_ = nullptr;
        emit_lines();
    }
}

Model::~Model() {
    close();
    if (library_)
        dlclose(library_);
}

uint64_t Model::time() const {
    return api_->time(model_);
}

int Model::precision() const {
    return api_->precision(model_);
}

bool Model::next_event_time(uint64_t& t) const {
    return !finished_ && api_->next_event(model_, &t);
}

void Model::get(const Port& p, uint8_t* buf) const {
    const size_t n = (p.size + 31) / 32;
    words_.resize(n);
    api_->get(model_, (unsigned)p.index, words_.data());
    auto* w = reinterpret_cast<VlogLogicVal*>(buf);
    for (size_t i = 0; i < n; i++)
        w[i] = {words_[i], 0};
}

void Model::put(const Port& p, const uint8_t* buf) {
    const size_t n = (p.size + 31) / 32;
    words_.resize(n);
    const auto* w = reinterpret_cast<const VlogLogicVal*>(buf);
    for (size_t i = 0; i < n; i++)
        words_[i] = w[i].aVal & ~w[i].bVal; // X and Z read as 0
    if (n && p.size % 32)
        words_[n - 1] &= (1u << (p.size % 32)) - 1;
    api_->put(model_, (unsigned)p.index, words_.data());
    dirty_ = true;
}

Core::RunResult Model::eval() {
    if (api_->eval(model_) != 0)
        failed_ = true;
    dirty_ = false;
    emit_lines();
    if (failed_)
        return {RunStatus::Error, "Verilator's runtime stopped the model"};
    if (api_->finished(model_)) {
        finished_ = true;
        return {RunStatus::Finished, "simulation finished"};
    }
    return {RunStatus::Completed, ""};
}

// The model's own events up to the target are evaluated at their times (the
// bridge visits each, so normally there is none before it), then the target
// time slot, if anything is due there: an event, or the inputs written.
Core::RunResult Model::start(uint64_t ticks) {
    if (failed_)
        return {RunStatus::Error, "Verilator's runtime stopped the model"};
    if (finished_)
        return {RunStatus::Finished, "simulation finished"};
    const uint64_t target = time() + ticks;
    RunResult r{RunStatus::Completed, ""};
    uint64_t t;
    while (api_->next_event(model_, &t) && t <= target) {
        if (t > time())
            api_->set_time(model_, t);
        r = eval();
        if (r.status != RunStatus::Completed)
            return r;
    }
    if (time() < target)
        api_->set_time(model_, target);
    if (dirty_)
        r = eval();
    return r;
}

Core::RunResult Model::resume() {
    // never suspended: start() runs a time slot to convergence
    return {RunStatus::Completed, ""};
}

Model::Line Model::parse_line(const std::string& line) {
    Line l;
    size_t pos = 0;
    bool timed = false; // a message of $info and friends, after its time
    if (line.starts_with('[')) {
        size_t end = line.find("] ");
        if (end == std::string::npos)
            return l;
        pos = end + 2;
        timed = true;
    }
    static const struct {
        const char* prefix;
        Severity severity;
    } kinds[] = {
        {"-Info", Severity::Note},
        {"%Info", Severity::Note},
        {"%Warning", Severity::Warning},
        {"%Error", Severity::Error},
        {"%Fatal", Severity::Failure},
    };
    for (const auto& k : kinds) {
        const size_t n = strlen(k.prefix);
        if (line.compare(pos, n, k.prefix) != 0)
            continue;
        // "%Error: " or, with a code, "%Error-CODE: "
        size_t colon = line.find(": ", pos + n);
        if (colon == std::string::npos)
            return l;
        for (size_t i = pos + n; i < colon; i++)
            if (!(line[i] == '-' || isupper((unsigned char)line[i])))
                return l;
        pos = colon + 2;
        l.report = true;
        l.severity = k.severity;
        break;
    }
    if (!l.report)
        return l;
    pos += location_length(line, pos);
    std::string text = line.substr(pos);
    if (timed) {
        // "Assertion failed in top.sub: text" or "top.sub: text": the
        // scope, which the bridge's report names for itself
        static const std::string failed = "Assertion failed in ";
        if (text.starts_with(failed))
            text.erase(0, failed.size());
        size_t sep = text.find(": ");
        if (sep != std::string::npos)
            text.erase(0, sep + 2);
    }
    l.text = std::move(text);
    return l;
}

// Lines wait for the call into the model to return: a report's sink may not
// return either (NVC ends the run at a failure), and must not unwind through
// Verilator's evaluation, its coroutines included.
void Model::on_print(void* user, const char* line) {
    static_cast<Model*>(user)->lines_.emplace_back(line);
}

void Model::emit_lines() {
    // (taken first: a report may not return)
    std::vector<std::string> lines;
    lines.swap(lines_);
    for (const std::string& line : lines) {
        Line l = parse_line(line);
        if (l.report) {
            report(l.severity, l.text, line);
        } else {
            // $display and the like: the model's own output, as it would be
            // standalone
            fputs(line.c_str(), stdout);
            fputc('\n', stdout);
            fflush(stdout);
        }
    }
}

} // namespace nvcx::verilator
