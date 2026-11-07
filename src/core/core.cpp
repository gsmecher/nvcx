#include "nvcx/core.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

namespace nvcx {

const Core::Port* Core::port(const std::string& name) const {
    for (const Port& p : ports_)
        if (p.name == name)
            return &p;
    return nullptr;
}

Core::RunResult Core::run(uint64_t ticks) {
    RunResult r = start(ticks);
    while (r.status == RunStatus::Yielded)
        r = resume();
    return r;
}

void Core::report(Severity s,
                  const std::string& text,
                  const std::string& formatted) {
    if (report_sink_) {
        report_sink_(s, text);
    } else {
        fprintf(stdout, "%s\n", formatted.c_str());
        fflush(stdout);
    }
}

std::string vlog_to_logic(const uint8_t* raw, unsigned nbits) {
    std::string out(nbits, 'X');
    for (unsigned i = 0; i < nbits; i++) {
        VlogLogicVal w;
        std::memcpy(&w, raw + (i / 32) * sizeof(VlogLogicVal), sizeof w);
        bool a = (w.aVal >> (i % 32)) & 1, b = (w.bVal >> (i % 32)) & 1;
        out[nbits - 1 - i] = b ? (a ? 'X' : 'Z') : (a ? '1' : '0');
    }
    return out;
}

void logic_to_vlog(const std::string& s, uint8_t* raw, unsigned nbits) {
    const size_t words = (nbits + 31) / 32;
    std::vector<VlogLogicVal> w(words, VlogLogicVal{0, 0});
    for (unsigned i = 0; i < nbits && i < s.size(); i++) {
        char c = toupper((unsigned char)s[s.size() - 1 - i]);
        bool a, b;
        if (c == '0' || c == 'L') {
            a = 0;
            b = 0;
        } else if (c == '1' || c == 'H') {
            a = 1;
            b = 0;
        } else if (c == 'Z') {
            a = 0;
            b = 1;
        } else {
            a = 1;
            b = 1;
        }
        if (a)
            w[i / 32].aVal |= 1u << (i % 32);
        if (b)
            w[i / 32].bVal |= 1u << (i % 32);
    }
    std::memcpy(raw, w.data(), words * sizeof(VlogLogicVal));
}

} // namespace nvcx
