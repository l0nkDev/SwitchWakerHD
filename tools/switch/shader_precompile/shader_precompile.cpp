// shader_precompile: translates every shader combination of a manifest (shadercache_manifest.bin, recorded on the
// console: gfx/deko/shaders_dk.cpp manifest_record) to GLSL again, from the game's own programs, and writes the
// sources as a WGS1 shadercache_gl.bin; tools/switch/dksh_cache compiles that into the shadercache_dksh.bin the
// console loads at start, so none of these shaders is compiled on the console.
//   shader_precompile <shadercache_manifest.bin> <programs.bin> <out shadercache_gl.bin>
// programs.bin: the game's programs the manifest names, found in its files (collect_programs.py): records
// {u64 hash, u32 size, bytes}, little-endian.
// Each combination is translated exactly as gfx/deko/shaders_dk.cpp decompile() does (the decompiler's OpenGL mode,
// the same fetch shaders, LatteShader_UpdatePSInputs, the program hash as the decompiler's base), and its GLSL hash
// must equal the one the console recorded: a combination whose GLSL differs is left out (and counted).
#include <sys/mman.h>
#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <vector>

#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.h"
#include "gx2/gx2.h"
#include "ppc.h"
#include "util/helpers/StringBuf.h"

LatteDecompilerShader* FinishDecompiledShader(LatteDecompilerOutput_t& output);
LatteFetchShader* LatteShaderRecompiler_createFetchShader(LatteFetchShader::CacheHash hash, uint32* regs, uint32* code,
                                                          uint32 size);

std::unique_ptr<Renderer> g_renderer = std::make_unique<OpenGLRenderer>();  // (deko3d: the OpenGL mode)
static bool g_verbose = false;
void cemu_shim_log(const std::string& msg) {
    if (g_verbose) fprintf(stderr, "[decompiler] %s\n", msg.c_str());
}

namespace {
// gfx/deko/shaders_dk.cpp hash_bytes
uint64_t hash_bytes(const void* bytes, size_t size, uint64_t hash = 0x9E3779B97F4A7C15ull) {
    const auto* p = static_cast<const uint8_t*>(bytes);
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t word;
        memcpy(&word, p + i, 8);
        hash = (hash ^ word) * 0xFF51AFD7ED558CCDull;
        hash ^= hash >> 32;
    }
    for (; i < size; ++i) hash = (hash ^ p[i]) * 0x100000001B3ull;
    return hash ^ (hash >> 29);
}

std::vector<uint8_t> read_file(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
}

struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    template <class T> T get() {
        T v{};
        if (p + sizeof v <= end) memcpy(&v, p, sizeof v);
        p += sizeof v;
        return v;
    }
    bool ok() const { return p <= end; }
};

constexpr uint32_t kProgramAddress = 0x10000000;  // where a program is put in guest memory (256-aligned)
constexpr uint32_t kFetchAddress = 0x10200000;
}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: shader_precompile <shadercache_manifest.bin> <programs.bin> <out shadercache_gl.bin> [-v]\n");
        return 2;
    }
    g_verbose = argc > 4 && !strcmp(argv[4], "-v");
    // guest memory at the port's fixed base, as the console's (ppc.h PPC_MEM_BASE); only the first pages are touched
    void* mem = mmap(PPC_MEM_BASE, 0x100000000ull, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
    if (mem != (void*)PPC_MEM_BASE) {
        fprintf(stderr, "cannot map guest memory at %p\n", (void*)PPC_MEM_BASE);
        return 1;
    }

    std::unordered_map<uint64_t, std::vector<uint8_t>> programs;
    {
        std::vector<uint8_t> d = read_file(argv[2]);
        Reader r{d.data(), d.data() + d.size()};
        while (r.p + 12 <= r.end) {
            const uint64_t h = r.get<uint64_t>();
            const uint32_t size = r.get<uint32_t>();
            if (r.p + size > r.end) break;
            programs[h].assign(r.p, r.p + size);
            r.p += size;
        }
    }

    std::vector<uint8_t> manifest = read_file(argv[1]);
    std::unordered_map<uint64_t, std::vector<uint8_t>> fetchBytes;
    std::vector<uint32_t> regs(LATTE_MAX_REGISTER);
    std::vector<uint8_t> out = {'W', 'G', 'S', '1'};
    std::unordered_map<uint64_t, bool> written;  // GLSL hash -> written
    size_t records = 0, same = 0, differ = 0, noProgram = 0, noFetch = 0, failed = 0, duplicate = 0;
    Reader r{manifest.data(), manifest.data() + manifest.size()};
    while (r.p < r.end) {
        const uint8_t type = r.get<uint8_t>();
        if (type == 2) {
            const uint64_t key = r.get<uint64_t>();
            const uint32_t size = r.get<uint32_t>();
            if (r.p + size > r.end) break;
            fetchBytes[key].assign(r.p, r.p + size);
            r.p += size;
            continue;
        }
        if (type != 1 && type != 3) {
            fprintf(stderr, "manifest: unknown record type %u at %zu\n", type, size_t(r.p - 1 - manifest.data()));
            return 1;
        }
        records++;
        const bool vertex = r.get<uint8_t>() != 0;
        const uint64_t programHash = r.get<uint64_t>();
        const uint32_t size = r.get<uint32_t>();
        if (type == 3) r.get<uint64_t>();  // head hash (finding the program: collect_programs.py)
        const uint64_t glslHash = r.get<uint64_t>();
        const uint64_t fsKey = r.get<uint64_t>();
        const uint16_t n = r.get<uint16_t>();
        std::fill(regs.begin(), regs.end(), 0);
        for (uint16_t i = 0; i < n; i++) {
            const uint16_t reg = r.get<uint16_t>();
            regs[reg] = r.get<uint32_t>();
        }
        if (!r.ok()) break;
        if (written.count(glslHash)) {  // another combination with the same GLSL: compiled once
            duplicate++;
            continue;
        }
        auto prog = programs.find(programHash);
        if (prog == programs.end() || prog->second.size() != size) {
            noProgram++;
            continue;
        }
        memcpy(ppc_ptr(kProgramAddress), prog->second.data(), size);
        regs[vertex ? mmSQ_PGM_START_VS : mmSQ_PGM_START_PS] = kProgramAddress >> 8;
        LatteFetchShader* fetch = nullptr;
        if (vertex) {
            auto fb = fetchBytes.find(fsKey);
            if (fb == fetchBytes.end()) {
                noFetch++;
                continue;
            }
            memcpy(ppc_ptr(kFetchAddress), fb->second.data(), fb->second.size());
            regs[mmSQ_PGM_START_FS] = kFetchAddress >> 8;
            const bool compact = ld32(kFetchAddress) == gx2::kFetchMagic;  // (shaders_dk.cpp fetch_shader_range)
            fetch = compact ? gx2::build_fetch_shader(kFetchAddress)
                            : LatteShaderRecompiler_createFetchShader(fsKey, regs.data(),
                                                                      reinterpret_cast<uint32_t*>(ppc_ptr(kFetchAddress)),
                                                                      uint32_t(fb->second.size()));
            if (!fetch) {
                noFetch++;
                continue;
            }
        }
        // shaders_dk.cpp decompile()
        if ((regs[Latte::REGADDR::VGT_GS_MODE] & 3) != 0) {
            failed++;
            continue;
        }
        LatteShader_UpdatePSInputs(regs.data());
        LatteDecompilerOptions options;
        LatteDecompilerOutput_t output{};
        const uint64_t base = programHash ^ (vertex ? 0x1111 : 0x2222);
        if (vertex)
            LatteDecompiler_DecompileVertexShader(base, regs.data(), ppc_ptr(kProgramAddress), size, fetch, options, &output);
        else
            LatteDecompiler_DecompilePixelShader(base, regs.data(), ppc_ptr(kProgramAddress), size, options, &output);
        if (!output.shader || output.shader->hasError || !output.shader->strBuf_shaderSource) {
            failed++;
            continue;
        }
        LatteDecompilerShader* sh = FinishDecompiledShader(output);
        const std::string glsl = sh->strBuf_shaderSource->c_str();
        const uint64_t h = hash_bytes(glsl.data(), glsl.size(), vertex ? 0x1111 : 0x2222);
        if (h != glslHash) {
            if (g_verbose && differ < 3)
                fprintf(stderr, "%s %016llx: GLSL %016llx, the console's %016llx\n", vertex ? "vertex" : "pixel",
                        (unsigned long long)programHash, (unsigned long long)h, (unsigned long long)glslHash);
            differ++;
            continue;
        }
        same++;
        written[h] = true;
        // WGS1 record 1 (shaders_dk.cpp cache_gl_source)
        uLongf packed = compressBound(glsl.size());
        std::vector<uint8_t> rec(18 + packed);
        if (compress2(rec.data() + 18, &packed, (const Bytef*)glsl.data(), glsl.size(), 6) != Z_OK) continue;
        const uint32_t sizes[2] = {uint32_t(packed), uint32_t(glsl.size())};
        rec[0] = 1;
        rec[1] = vertex;
        memcpy(rec.data() + 2, &h, 8);
        memcpy(rec.data() + 10, sizes, 8);
        out.insert(out.end(), rec.begin(), rec.begin() + 18 + packed);
    }
    FILE* f = fopen(argv[3], "wb");
    if (!f || fwrite(out.data(), 1, out.size(), f) != out.size()) {
        fprintf(stderr, "cannot write %s\n", argv[3]);
        return 1;
    }
    fclose(f);
    printf("%zu combinations: %zu translated to the console's GLSL, %zu with the same GLSL as another, %zu different "
           "GLSL, %zu without their program, %zu without their fetch shader, %zu failed; %zu sources in %s\n",
           records, same, duplicate, differ, noProgram, noFetch, failed, written.size(), argv[3]);
    return differ || failed ? 3 : 0;
}
