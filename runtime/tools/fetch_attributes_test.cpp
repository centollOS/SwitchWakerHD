// Synthetic layouts only. No driver, game data or guest address space.
#include "gx2/fetch_attributes.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include <cassert>
#include <cstring>
#include <memory>
#include <vector>
std::unique_ptr<Renderer> g_renderer=std::make_unique<Renderer>(RendererAPI::Vulkan);
LatteFetchShader::~LatteFetchShader() {}
namespace {
using Bytes=std::vector<uint8_t>;
void le(Bytes& b,uint32_t v) { for(int i=0;i<4;++i)b.push_back(uint8_t(v>>(8*i))); }
void be(Bytes& b,uint32_t v) { for(int i=3;i>=0;--i)b.push_back(uint8_t(v>>(8*i))); }
uint64_t native_hash(const Bytes& b) {
    uint64_t h=0x9e3779b97f4a7c15ull;size_t i=0;
    for(;i+8<=b.size();i+=8) { uint64_t w;memcpy(&w,b.data()+i,8);h=(h^w)*0xff51afd7ed558ccdull;h^=h>>32; }
    for(;i<b.size();++i)h=(h^b[i])*0x100000001b3ull;
    return h^(h>>29);
}
Bytes semantic(const Bytes& raw,bool compact,const std::vector<std::array<uint32_t,13>>& attrs) {
    Bytes b={'W','W','F','A','0','0','0','1'};auto hash=native_hash(raw);
    le(b,uint32_t(hash));le(b,uint32_t(hash>>32));le(b,attrs.size());le(b,compact);
    le(b,uint32_t(hash));le(b,uint32_t(hash>>32));
    for(auto& a:attrs)for(auto w:a)le(b,w);
    return b;
}
void compare(const LatteFetchShader* a,const LatteFetchShader* b) {
    assert(a&&b);assert(a->attributeBufferMask==b->attributeBufferMask);
    auto groups=[](const auto& x,const auto& y) {
        assert(x.size()==y.size());
        for(size_t i=0;i<x.size();++i) {
            assert(x[i].attributeBufferIndex==y[i].attributeBufferIndex);
            assert(x[i].attribCount==y[i].attribCount);
            assert(x[i].minOffset==y[i].minOffset);
            assert(x[i].totalAttribRangeSize==y[i].totalAttribRangeSize);
            assert(x[i].hasVtxIndexAccess==y[i].hasVtxIndexAccess);
            assert(x[i].hasInstanceIndexAccess==y[i].hasInstanceIndexAccess);
            for(int j=0;j<x[i].attribCount;++j)
                assert(!memcmp(&x[i].attrib[j],&y[i].attrib[j],sizeof(LatteParsedFetchShaderAttribute)));
        }
    };
    groups(a->bufferGroups,b->bufferGroups);groups(a->bufferGroupsInvalid,b->bufferGroupsInvalid);
    assert(a->vkPipelineHashFragment==b->vkPipelineHashFragment);
}
void compact_parity() {
    Bytes raw;be(raw,0x57574653);be(raw,3);be(raw,0);be(raw,0);
    // Descending buffers, resolved default endian, instancing and invalid buffer.
    for(auto a:std::vector<std::array<uint32_t,4>>{{1|(2<<8)|(1<<16)|(3u<<24),4,7,0x00010203},
        {2|(3u<<24),0,0,0x04050607},{3|(20<<8),0,0,0}})for(auto w:a)be(raw,w);
    auto* ordinary=gx2::build_fetch_shader_bytes(raw,123);
    auto desc=semantic(raw,true,{{0,2,1,0,0,0,0,4,5,6,7,0,0},
                               {2,1,15,1,0,0,1,0,1,2,3,4,1}});
    uint64_t hash=0;auto* prebuilt=gx2::decode_fetch_attributes_bytes(desc,&hash);
    assert(hash==native_hash(raw));
    // Compact path keeps its historical zero fragment; compare normalized
    // fields without altering that representation-specific identity.
    gx2::normalize_fetch_shader(ordinary);ordinary->vkPipelineHashFragment=0;
    compare(ordinary,prebuilt);
    for(size_t n=0;n<desc.size();++n)
        assert(!gx2::decode_fetch_attributes_bytes({desc.data(),n}));
    auto bad=desc;bad[20]=2;assert(!gx2::decode_fetch_attributes_bytes(bad));
    bad=desc;bad[32+12*4]=0; // vertex divisor0 remains valid
    assert(gx2::decode_fetch_attributes_bytes(bad));
    bad=desc;bad[32+52+12*4]=0;assert(!gx2::decode_fetch_attributes_bytes(bad));
    assert(!gx2::build_fetch_shader_bytes({raw.data(),raw.size()-1},123));
}
void legacy_parity() {
    std::vector<uint32_t> regs(65536);regs[0xa2a8]=7;
    // Synthetic VTX semantic, register-selected instance divisor, endian U32.
    std::vector<uint32_t> words={1|(1<<5)|(160<<8)|(1<<24),
        5|(1<<9)|(2<<12)|(3<<15)|(4<<18)|(1<<22),8|(2<<16),0};
    Bytes raw;for(auto w:words)le(raw,w);
    auto* ordinary=LatteShaderRecompiler_createFetchShader(native_hash(raw),regs.data(),words.data(),raw.size());
    gx2::normalize_fetch_shader(ordinary);
    auto desc=semantic(raw,false,{{0,5,1,1,0,0,2,1,2,3,4,8,7}});
    auto* prebuilt=gx2::decode_fetch_attributes_bytes(desc);compare(ordinary,prebuilt);
    assert(prebuilt->vkPipelineHashFragment!=0);
    // Normalization must erase ABI padding, independently of allocation bytes.
    auto clean=ordinary->bufferGroups[0].attrib[0];
    auto* bytes=reinterpret_cast<uint8_t*>(&ordinary->bufferGroups[0].attrib[0]);
    std::vector<bool> semanticBytes(sizeof(clean),false);
#define FIELD(f) for(size_t i=offsetof(LatteParsedFetchShaderAttribute,f);i<offsetof(LatteParsedFetchShaderAttribute,f)+sizeof(clean.f);++i)semanticBytes[i]=true
    FIELD(attributeBufferIndex);FIELD(semanticId);FIELD(format);FIELD(fetchType);FIELD(nfa);FIELD(isSigned);FIELD(endianSwap);FIELD(ds);FIELD(aluDivisor);FIELD(offset);
#undef FIELD
    for(size_t i=0;i<semanticBytes.size();++i)if(!semanticBytes[i])bytes[i]=0xa5;
    gx2::normalize_fetch_shader(ordinary);compare(ordinary,prebuilt);
}
}
int main() {
    assert(native_hash(Bytes{'a'})==0x22c04a325d932886ull);
    assert(native_hash(Bytes{'a','b','c','d','e','f','g','h'})==0x82ddd764292bbe85ull);
    assert(native_hash(Bytes{'a','b','c','d','e','f','g','h','i'})==0x23fae58d441e3dd5ull);
    compact_parity();legacy_parity();
}
