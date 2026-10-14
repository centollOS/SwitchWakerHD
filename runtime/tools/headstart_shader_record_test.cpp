#include "gfx/vulkan/headstart_shader_record.h"
#include <cassert>
#include <vector>
template<class F> void rejects(F f) { bool bad=false; try { f(); } catch(const std::runtime_error&) { bad=true; } assert(bad); }
int main() {
  std::vector<uint8_t> bytes;
  auto word=[&](uint32_t value) { for(int i=0;i<4;++i) bytes.push_back(value>>(8*i)); };
  word(0);word(8);word(0);word(1);word(8);
  bytes.insert(bytes.end(),8,0x11); bytes.insert(bytes.end(),8,0x22);word(0xa21a);word(0);
  auto record=gfxvk::decode_headstart_shader_record(bytes,6);
  assert(!record.vertex && record.program.size()==8 && record.program[0]==0x11);
  assert(record.linkedVertex.size()==8 && record.linkedVertex[0]==0x22 && record.registers.size()==8);
  rejects([&] { gfxvk::decode_headstart_shader_record(bytes,1); });
  auto bad=bytes;bad[0]=1;rejects([&] { gfxvk::decode_headstart_shader_record(bad,6); });
  bad=bytes;bad[16]=0;rejects([&] { gfxvk::decode_headstart_shader_record(bad,6); });
  bad=bytes;bad.pop_back();rejects([&] { gfxvk::decode_headstart_shader_record(bad,6); });
  bad=bytes;bad.push_back(0);rejects([&] { gfxvk::decode_headstart_shader_record(bad,6); });
  bad=bytes;bad[12]=0xff;bad[13]=0xff;bad[14]=0xff;bad[15]=0xff;
  rejects([&] { gfxvk::decode_headstart_shader_record(bad,6); });
}
