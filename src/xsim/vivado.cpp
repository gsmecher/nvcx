#include "nvcx/xsim/vivado.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <elf.h>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace nvcx::xsim {

namespace {

    const char* const SHARED_WITH_HOST[] = {"libstdc++.so",
                                            "libgcc_s.so",
                                            "libc.so",
                                            "libm.so",
                                            "libdl.so",
                                            "libpthread.so",
                                            "librt.so",
                                            "libz.so",
                                            "libutil.so"};
    // The prefix of the kernel stub's placeholder NEEDED entry (CMakeLists)
    const char* const KERNEL_PATH_PLACEHOLDER = "nvcx-kernel-path-";

    bool host_provided(const std::string& n) {
        for (const char* p : SHARED_WITH_HOST)
            if (n.rfind(p, 0) == 0)
                return true;
        return false;
    }

    std::vector<char> slurp(const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        if (!f)
            throw std::runtime_error("cannot read " + p.string());
        return std::vector<char>((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
    }

    // $XILINX_VIVADO, or an error.
    fs::path vivado_dir() {
        const char* v = getenv("XILINX_VIVADO");
        if (!v || !*v)
            throw std::runtime_error(
                "XILINX_VIVADO is not set (source a Vivado settings64.sh)");
        return v;
    }

    // vivado_dir()/lib/lnx64.o, where the simulator's libraries are.
    fs::path vivado_libdir() {
        return vivado_dir() / "lib" / "lnx64.o";
    }

    // Vivado's simulator kernel, libxv_simulator_kernel.so.
    fs::path vivado_kernel() {
        fs::path libdir = vivado_libdir();
        std::string best;
        if (fs::is_directory(libdir))
            for (auto& e : fs::directory_iterator(libdir)) {
                std::string n = e.path().filename().string();
                if (n.find("_simulator_kernel.so") != std::string::npos &&
                    (best.empty() || n < best))
                    best = n;
            }
        if (best.empty())
            throw std::runtime_error("no simulator kernel in " +
                                     libdir.string());
        return libdir / best;
    }

    // Directory holding this library (the stub is installed beside it).
    fs::path self_dir() {
        Dl_info info{};
        if (dladdr(reinterpret_cast<void*>(&self_dir), &info) && info.dli_fname)
            return fs::path(info.dli_fname).parent_path();
        return fs::read_symlink("/proc/self/exe").parent_path();
    }

    // One of nvcx's stub libraries, *name*, beside this library or
    // executable (the build tree) or in lib/nvcx of the install.
    fs::path stub_path(const std::string& name) {
        for (const fs::path& cand :
             {self_dir() / name, self_dir() / ".." / "lib" / "nvcx" / name})
            if (fs::is_regular_file(cand))
                return cand;
        throw std::runtime_error(std::format(
            "stub {} not found next to {}", name, self_dir().string()));
    }

    // The dynamic section of a shared library, with its string table.
    struct ElfDynamic {
        std::vector<char> data;
        const Elf64_Dyn* dyn = nullptr;
        size_t count = 0;
        uint64_t strtab = 0; // file offset
        std::vector<Elf64_Dyn> entries() const {
            std::vector<Elf64_Dyn> out;
            for (size_t i = 0; i < count && dyn[i].d_tag != DT_NULL; i++)
                out.push_back(dyn[i]);
            return out;
        }
        const char* string(uint64_t off) const {
            return data.data() + strtab + off;
        }
    };

    ElfDynamic read_dynamic(const fs::path& so) {
        ElfDynamic e;
        e.data = slurp(so);
        std::vector<char>& d = e.data;
        if (d.size() < sizeof(Elf64_Ehdr) ||
            std::memcmp(d.data(), ELFMAG, SELFMAG) != 0 ||
            d[EI_CLASS] != ELFCLASS64)
            throw std::runtime_error(so.string() + ": not an ELF64 file");
        auto* eh = reinterpret_cast<const Elf64_Ehdr*>(d.data());
        auto* ph = reinterpret_cast<const Elf64_Phdr*>(d.data() + eh->e_phoff);
        const Elf64_Phdr* dyn = nullptr;
        for (unsigned i = 0; i < eh->e_phnum; i++)
            if (ph[i].p_type == PT_DYNAMIC)
                dyn = &ph[i];
        if (!dyn)
            throw std::runtime_error(so.string() + ": no PT_DYNAMIC");
        auto vaddr_to_off = [&](uint64_t va) -> uint64_t {
            for (unsigned i = 0; i < eh->e_phnum; i++)
                if (ph[i].p_type == PT_LOAD && va >= ph[i].p_vaddr &&
                    va < ph[i].p_vaddr + ph[i].p_filesz)
                    return ph[i].p_offset + (va - ph[i].p_vaddr);
            throw std::runtime_error(so.string() + ": bad vaddr");
        };
        e.dyn = reinterpret_cast<const Elf64_Dyn*>(d.data() + dyn->p_offset);
        e.count = dyn->p_filesz / sizeof(Elf64_Dyn);
        for (const Elf64_Dyn& x : e.entries())
            if (x.d_tag == DT_STRTAB)
                e.strtab = vaddr_to_off(x.d_un.d_ptr);
        return e;
    }

    struct ElfInfo {
        std::vector<std::string> needed;
        std::string soname;
    };

    ElfInfo elf_info(const fs::path& so) {
        ElfDynamic elf = read_dynamic(so);
        ElfInfo info;
        for (const Elf64_Dyn& d : elf.entries()) {
            if (d.d_tag == DT_NEEDED)
                info.needed.push_back(elf.string(d.d_un.d_val));
            else if (d.d_tag == DT_SONAME)
                info.soname = elf.string(d.d_un.d_val);
        }
        return info;
    }

    // The kernel stub's image, NEEDing *kernel* in place of its placeholder
    std::vector<char> kernel_stub_image(const fs::path& stub,
                                        const fs::path& kernel) {
        ElfDynamic elf = read_dynamic(stub);
        const std::string path = fs::absolute(kernel).string();
        for (const Elf64_Dyn& d : elf.entries()) {
            if (d.d_tag != DT_NEEDED)
                continue;
            const char* s = elf.string(d.d_un.d_val);
            if (!std::string_view(s).starts_with(KERNEL_PATH_PLACEHOLDER))
                continue;
            if (path.size() > strlen(s))
                throw std::runtime_error(std::format(
                    "the kernel's path {} is longer than {} has room for",
                    path,
                    stub.string()));
            std::memcpy(elf.data.data() + (s - elf.data.data()),
                        path.c_str(),
                        path.size() + 1);
            return std::move(elf.data);
        }
        throw std::runtime_error(stub.string() +
                                 " has no placeholder for the kernel's path");
    }

    // Load a soname-less kernel's stub (see src/kernelstub.cpp), with its
    // path written in, from memory. The loader knows the stub by its
    // /proc/self/fd path, so the descriptor stays open: that path must never
    // name another file.
    void load_kernel_stub(const fs::path& kernel) {
        const std::string name = kernel.filename().string();
        const fs::path stub = stub_path(name);
        if (elf_info(stub).soname != name)
            throw std::runtime_error(std::format(
                "{} does not carry the soname {}", stub.string(), name));
        std::vector<char> image = kernel_stub_image(stub, kernel);
        int fd = memfd_create(name.c_str(), MFD_CLOEXEC);
        if (fd < 0)
            throw std::runtime_error(
                std::format("memfd_create: {}", strerror(errno)));
        for (size_t off = 0; off < image.size();) {
            ssize_t n = write(fd, image.data() + off, image.size() - off);
            if (n < 0)
                throw std::runtime_error(std::format(
                    "writing the kernel stub: {}", strerror(errno)));
            off += (size_t)n;
        }
        const std::string proc = std::format("/proc/self/fd/{}", fd);
        if (!dlopen(proc.c_str(), RTLD_LAZY | RTLD_LOCAL | RTLD_DEEPBIND))
            throw std::runtime_error(
                std::format("cannot load the kernel stub for {}: {}",
                            kernel.string(),
                            dlerror()));
    }

} // namespace

const Closure& load_closure() {
    static std::mutex mu;
    static std::optional<Closure> loaded;
    std::lock_guard<std::mutex> lock(mu);
    if (loaded)
        return *loaded;
    Closure c;
    c.kernel = vivado_kernel();
    // Transitive closure of the kernel's NEEDED entries within Vivado's
    // library directories, minus what the host provides.
    const fs::path search[] = {vivado_libdir(), vivado_libdir() / "Ubuntu"};
    std::map<std::string, fs::path> found;
    std::vector<fs::path> todo{c.kernel};
    while (!todo.empty()) {
        fs::path lib = todo.back();
        todo.pop_back();
        for (const std::string& n : elf_info(lib).needed) {
            if (found.count(n) || host_provided(n))
                continue;
            for (const fs::path& d : search)
                if (fs::is_regular_file(d / n)) {
                    found[n] = d / n;
                    todo.push_back(d / n);
                    break;
                }
        }
    }
    // The bridge library (lib{xv,rdi}_simbridge_kernel.so, after the
    // kernel's name): Vivado's own only when the closure imports from it.
    // Designs with IP NEED it too.
    std::string bridge_name = c.kernel.filename().string();
    bridge_name.replace(bridge_name.find("simulator_kernel"),
                        std::string_view("simulator_kernel").size(),
                        "simbridge_kernel");
    auto bridge = found.find(bridge_name);
    if (bridge != found.end()) {
        c.bridge = bridge->second;
        found.erase(bridge);
    } else {
        c.bridge = stub_path(bridge_name);
        if (elf_info(c.bridge).soname != bridge_name)
            throw std::runtime_error(
                std::format("{} does not carry the soname {}",
                            c.bridge.string(),
                            bridge_name));
    }
    std::vector<fs::path> pending{c.bridge};
    for (auto& [name, path] : found)
        pending.push_back(path);
    // Each library needs its own dependencies loaded first (there is no
    // rpath): passes until no progress. (Vivado's libgomp uses static TLS,
    // which dlopen serves from glibc's surplus; one such library fits with
    // room to spare, and dlopen says so clearly if that ever changes.)
    std::string last_error;
    while (!pending.empty()) {
        std::vector<fs::path> next;
        for (const fs::path& p : pending) {
            if (dlopen(p.c_str(), RTLD_LAZY | RTLD_LOCAL | RTLD_DEEPBIND))
                continue;
            last_error = dlerror();
            next.push_back(p);
        }
        if (next.size() == pending.size())
            throw std::runtime_error(
                std::format("cannot load the kernel's library closure from "
                            "{}: {}",
                            vivado_libdir().string(),
                            last_error));
        pending.swap(next);
    }
    // Designs NEED the kernel by its file name, which before 2025.1 is not
    // its soname (it has none): a stub takes the name, and loads the kernel
    // as its dependency, in each design's own scope.
    if (elf_info(c.kernel).soname.empty())
        load_kernel_stub(c.kernel);
    loaded = c;
    return *loaded;
}

} // namespace nvcx::xsim
