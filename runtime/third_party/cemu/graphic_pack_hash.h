// Graphic pack shader hashes (base and auxiliary), as Cemu names replacement shaders.
// Adapted from Cemu src/Cafe/HW/Latte/Core/LatteShader.cpp
// Copyright (c) Cemu contributors. Licensed under the Mozilla Public License 2.0 (see LICENSE.txt).
#pragma once
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include <bit>
#include <cstring>
namespace cemu_pack_hash {
inline uint64_t program(const void* bytes, uint32_t size) {
    uint64_t a=0,b=0;
    for(uint32_t i=0;i<size/4;i++) {
        uint32_t word;std::memcpy(&word,static_cast<const uint8_t*>(bytes)+i*4,4);
        a=std::rotl(a+word,3);b=std::rotr(b^word,7);
    }
    return a+b;
}
// Vulkan variants without geometry shaders, matching Cemu's public pack names.
inline uint64_t base(const void* bytes,uint32_t size,const uint32_t* regs,bool vertex,const LatteFetchShader* fetch) {
    auto hash=program(bytes,size)+LatteSHRC_GetPSInputTable()->key;
    if(vertex) {
        hash+=fetch->key+(regs[Latte::REGADDR::PA_CL_VTE_CNTL]^0x43F);
        auto primitive=regs[Latte::REGADDR::VGT_PRIMITIVE_TYPE]&0x3f;
        if(primitive==0x11)hash+=13;else if(primitive==1)hash+=71;
        if(regs[mmVGT_STRMOUT_EN])hash+=21;
        if(regs[Latte::REGADDR::PA_CL_CLIP_CNTL]&(1u<<19))hash+=0x1537;
    }
    return hash;
}
inline uint64_t auxiliary(const LatteDecompilerShader& shader,const uint32_t* regs,bool vertex) {
    uint64_t hash=0;
    if(vertex) {
        if(shader.hasStreamoutBufferWrite)
            for(unsigned i=0;i<LATTE_NUM_STREAMOUT_BUFFER;i++)
                if(shader.streamoutBufferWriteMask[i])hash=std::rotl(hash,7)+regs[mmVGT_STRMOUT_VTX_STRIDE_0+i*4];
        uint64_t texture=0;
        for(unsigned i=0;i<shader.textureUnitListCount;i++)
            if((regs[Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS+shader.textureUnitList[i]*7+4]&0x300)==0x100)
                texture=std::rotl(texture,7)+0x333;
        return hash+texture;
    }
    hash=regs[mmCB_SHADER_MASK];auto alpha=regs[Latte::REGADDR::SX_ALPHA_TEST_CONTROL];
    if(alpha&8)hash=std::rotr(hash+(alpha&7),3)+1;
    for(unsigned i=0;i<shader.textureUnitListCount;i++)
        hash=std::rotl(hash,3)+(regs[Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS+shader.textureUnitList[i]*7]&7);
    return hash;
}
}
