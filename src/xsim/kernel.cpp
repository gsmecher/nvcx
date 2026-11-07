#define _GNU_SOURCE 1
#include "nvcx/xsim/kernel.hpp"
#include "nvcx/xsim/vivado.hpp"
#include <cxxabi.h>
#include <dlfcn.h>
#include <link.h>
#include <cstdlib>
#include <cstring>
#include <format>
#include <stdexcept>
#include <string_view>

namespace nvcx::xsim {

Kernel& Kernel::get() {
    static Kernel* kernel = new Kernel(load_closure().kernel.string());
    return *kernel;
}

Kernel::Kernel(const std::string& path) : path_(path) {
    // The kernel's libraries must already be in the process, since there is
    // no rpath: the loader then matches its NEEDED entries by soname.
    load_closure();
    dlerror();
    handle_ = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL | RTLD_DEEPBIND);
    if (!handle_)
        throw std::runtime_error(std::format(
            "cannot load simulator kernel {}: {}", path, dlerror()));

    // XSI's xsimDir, which lets nvcx open a snapshot wherever it is, arrived
    // in 2023: before, xsi_open looks only in the working directory's xsim.dir
    if (!dlsym(handle_, "iki_set_xsimdir_location_if_remapped"))
        throw std::runtime_error(
            std::format("simulator kernel {} is too old: its XSI cannot open "
                        "a snapshot outside the working directory (nvcx "
                        "supports Vivado 2023.2 and later)",
                        path));
    resolve_all();
    discover_offsets();
}

template <class F>
static void resolve(void* h,
                    F& fp,
                    const char* sym,
                    const char* expect_demangled = nullptr) {
    dlerror();
    void* p = dlsym(h, sym);
    if (!p)
        throw std::runtime_error(std::format(
            "kernel symbol missing: {}: {}", sym, dlerror() ? dlerror() : "?"));
    if (expect_demangled) {
        int st = 0;
        char* d = ::abi::__cxa_demangle(sym, nullptr, nullptr, &st);
        bool ok =
            st == 0 && d && std::string_view(d).starts_with(expect_demangled);
        std::string actual = d ? d : "";
        free(d);
        if (!ok)
            throw std::runtime_error(
                std::format("kernel symbol {} demangles to '{}', expected '{}'",
                            sym,
                            actual,
                            expect_demangled));
    }
    fp = reinterpret_cast<F>(p);
}

void Kernel::resolve_all() {
    void* h = handle_;
    resolve(h, xsi_close, "xsi_close");
    resolve(h, xsi_run, "xsi_run");
    resolve(h, xsi_put_value, "xsi_put_value");
    resolve(h, xsi_get_value, "xsi_get_value");
    resolve(h, xsi_get_status, "xsi_get_status");
    resolve(h, xsi_get_error_info, "xsi_get_error_info");
    resolve(h, xsi_get_int, "xsi_get_int");
    resolve(h, xsi_get_int_port, "xsi_get_int_port");
    resolve(h, xsi_get_str_port, "xsi_get_str_port");
    resolve(h, xsi_get_time, "xsi_get_time");

    resolve(h,
            host_isPortVHDL,
            "_ZN5ISIMK7XSIHost21isPortValueFormatVHDLEi",
            "ISIMK::XSIHost::isPortValueFormatVHDL(int)");

    resolve(h,
            host_setGlobals,
            "_ZN5ISIMK4Host10setGlobalsEb",
            "ISIMK::Host::setGlobals(bool)");

    // ISIMK::GlobalDesignProperties
    resolve(h, globalDesignProperties, "_ZN5ISIMK22GlobalDesignPropertiesE");

    // ISIMK::GlobalEventQueue
    resolve(h, globalEventQueue, "_ZN5ISIMK16GlobalEventQueueE");

    resolve(h,
            process_active_events,
            "_ZN5ISIMK6EventQ19processActiveEventsEv",
            "ISIMK::EventQ::processActiveEvents()");

    // optional: older kernels have no version string
    // ISIM::VersionUtils::populateVersion(char*, int)
    auto populateVersion = reinterpret_cast<void (*)(char*, int)>(
        dlsym(h, "_ZN4ISIM12VersionUtils15populateVersionEPci"));
    if (populateVersion) {
        char buf[64] = {};
        populateVersion(buf, sizeof buf);
        version = buf;
    }
}

bool Kernel::next_event_time(uint64_t& t) const {
    if (!globalEventQueue || !*globalEventQueue)
        return false;
    const char* q = static_cast<const char*>(*globalEventQueue);
    const char* slot =
        *reinterpret_cast<const char* const*>(q + abi::LISTQ_HEAD);
    if (!slot)
        return false;
    t = *reinterpret_cast<const uint64_t*>(slot + abi::TIMESLOT_TIME);
    return true;
}

void Kernel::hook_active_events(void (*hook)(void*)) {
    // The slot is the R_X86_64_JUMP_SLOT relocation for the symbol, found in
    // the loaded copy's dynamic section (the loader has already rebased the
    // table addresses).
    const char* const sym = "_ZN5ISIMK6EventQ19processActiveEventsEv";
    link_map* lm = nullptr;
    if (dlinfo(handle_, RTLD_DI_LINKMAP, &lm) != 0 || !lm)
        throw std::runtime_error("cannot read the kernel's link map");
    const ElfW(Rela)* rela = nullptr;
    size_t rela_size = 0;
    const ElfW(Sym)* symtab = nullptr;
    const char* strtab = nullptr;
    for (const ElfW(Dyn)* d = lm->l_ld; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_JMPREL:
            rela = reinterpret_cast<const ElfW(Rela)*>(d->d_un.d_ptr);
            break;
        case DT_PLTRELSZ:
            rela_size = d->d_un.d_val;
            break;
        case DT_SYMTAB:
            symtab = reinterpret_cast<const ElfW(Sym)*>(d->d_un.d_ptr);
            break;
        case DT_STRTAB:
            strtab = reinterpret_cast<const char*>(d->d_un.d_ptr);
            break;
        }
    }
    if (!rela || !symtab || !strtab)
        throw std::runtime_error("the kernel has no PLT relocations");
    for (size_t i = 0; i < rela_size / sizeof(ElfW(Rela)); i++) {
        if (ELF64_R_TYPE(rela[i].r_info) != R_X86_64_JUMP_SLOT)
            continue;
        const ElfW(Sym) & s = symtab[ELF64_R_SYM(rela[i].r_info)];
        if (std::strcmp(strtab + s.st_name, sym) != 0)
            continue;
        auto* slot = reinterpret_cast<void**>(lm->l_addr + rela[i].r_offset);
        *slot = reinterpret_cast<void*>(hook);
        return;
    }
    throw std::runtime_error(
        std::format("the kernel does not call {} through its PLT", sym));
}

void Kernel::discover_offsets() {
    // ISIMK::XSIHost::processStopEvent()
    auto* code = static_cast<const unsigned char*>(
        dlsym(handle_, "_ZN5ISIMK7XSIHost16processStopEventEv"));
    for (size_t i = 0; code && i + 10 <= 48; i++)
        if (code[i] == 0xc7 && code[i + 1] == 0x80) {
            uint32_t d, imm;
            std::memcpy(&d, code + i + 2, 4);
            std::memcpy(&imm, code + i + 6, 4);
            if (imm == 2 && d > 0x100 && d < 0x4000) {
                status_offset = d;
                break;
            }
        }
}

} // namespace nvcx::xsim
