// The port's encoded fetch "program" (GX2InitFetchShaderEx, gx2_resources.cpp) -> Cemu's LatteFetchShader. In a file of
// its own, with nothing but guest memory reads, so that the shader precompiler (tools/switch/shader_precompile) builds
// the same fetch shaders, and so the same vertex shader GLSL, as the console.
// Encoding: header (kFetchMagic, count, 0, 0) followed by 16 bytes per attribute:
//   +0 location | buffer << 8 | indexType << 16 | endianSwap << 24
//   +4 offset, +8 format | aluDivisor << 16, +C destSel
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "gx2.h"
#include "ppc.h"

namespace gx2 {
using Latte::E_HWFMT;

static LatteConst::VertexFetchEndianMode default_endian(uint32 fmt) {
    switch (fmt) {
    case 0: case 1: case 4: case 10: return LatteConst::VertexFetchEndianMode::SWAP_NONE;
    case 2: case 3: case 7: case 8: case 14: case 15: return LatteConst::VertexFetchEndianMode::SWAP_U16;
    default: return LatteConst::VertexFetchEndianMode::SWAP_U32;
    }
}
static const uint32 kRawToFetchFormat[] = {1, 2, 5, 6, 7, 0xD, 0xE, 0xF, 0x10, 0x16, 0x1A, 0x19, 0x1D, 0x1E, 0x1F, 0x20, 0x2F, 0x30, 0x22, 0x23};

LatteFetchShader* build_fetch_shader(uint32 program) {
    if (ld32(program) != kFetchMagic) return nullptr;
    uint32 n = ld32(program + 4);
    auto* fs = new LatteFetchShader();
    auto* attrs = new LatteParsedFetchShaderAttribute[n];
    std::vector<std::vector<LatteParsedFetchShaderAttribute*>> groups(16);
    for (uint32 i = 0; i < n; i++) {
        uint32 e = program + 16 + i * 16;
        uint32 w0 = ld32(e), offset = ld32(e + 4), w2 = ld32(e + 8), destSel = ld32(e + 12);
        uint32 location = w0 & 0xFF, buffer = (w0 >> 8) & 0xFF, indexType = (w0 >> 16) & 0xFF, endian = w0 >> 24;
        uint32 fmt = w2 & 0xFFFF, divisor = w2 >> 16;
        LatteParsedFetchShaderAttribute& a = attrs[i];
        a = {};
        a.attributeBufferIndex = (uint8)buffer;
        a.semanticId = (uint8)location;
        a.format = (E_HWFMT)(kRawToFetchFormat[std::min<uint32>(fmt & 0x3F, 19)] & 0x3F);
        a.nfa = (fmt & 0x800) ? 2 : (fmt & 0x100) ? 1 : 0;
        a.isSigned = (fmt & 0x200) ? 1 : 0;
        a.endianSwap = endian == 3 /* SWAP_DEFAULT */ ? default_endian(fmt & 0x3F) : (LatteConst::VertexFetchEndianMode)endian;
        a.fetchType = indexType ? LatteConst::VertexFetchType2::INSTANCE_DATA : LatteConst::VertexFetchType2::VERTEX_DATA;
        a.aluDivisor = indexType ? (sint32)std::max<uint32>(divisor, 1) : 0;
        a.offset = offset;
        for (int k = 0; k < 4; k++) a.ds[k] = (destSel >> (24 - 8 * k)) & 7;
        if (buffer < 16) groups[buffer].push_back(&a);
    }
    // attributes are grouped per buffer, contiguous in the attrs array order
    auto* sorted = new LatteParsedFetchShaderAttribute[n];
    uint32 k = 0;
    for (uint32 b = 0; b < 16; b++) {
        if (groups[b].empty()) continue;
        LatteParsedFetchShaderBufferGroup g{};
        g.attributeBufferIndex = (uint8)b;
        g.attribCount = (sint8)groups[b].size();
        g.attrib = &sorted[k];
        g.minOffset = 0xFFFFFFFF;
        for (auto* a : groups[b]) {
            sorted[k++] = *a;
            g.minOffset = std::min(g.minOffset, a->offset);
            if (a->fetchType == LatteConst::VertexFetchType2::VERTEX_DATA) g.hasVtxIndexAccess = true;
            else g.hasInstanceIndexAccess = true;
        }
        fs->bufferGroups.push_back(g);
        fs->attributeBufferMask |= 1u << b;
    }
    delete[] attrs;
    fs->key = program;
    return fs;
}
}  // namespace gx2
