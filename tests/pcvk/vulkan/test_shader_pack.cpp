// Offline shader pack (pcvk/graphics/shaders/shader_pack.*).
#include "../native/test_main.h"
#include "pcvk/graphics/shaders/shader_pack.h"
#include <cstring>
using namespace superman_returns::graphics;
using namespace superman_returns::graphics::shaders;
namespace {
void Put32(std::vector<uint8_t>& v,uint32_t x) {uint8_t b[4];std::memcpy(b,&x,4);v.insert(v.end(),b,b+4);}
// A minimal "SVR3" result blob: no interface, a 5-word SPIR-V header.
std::vector<uint8_t> Blob(ShaderStage stage,uint32_t marker) {
  std::vector<uint8_t> v;
  Put32(v,0x33525653);Put32(v,1);Put32(v,1);Put32(v,stage==ShaderStage::kVertex?0:1);
  for(int i=0;i<8;++i) Put32(v,i==0?marker:0);
  for(int i=0;i<4;++i) Put32(v,0);
  Put32(v,0);Put32(v,0);          // no input / output locations
  Put32(v,20);                    // SPIR-V bytes
  Put32(v,0x07230203);Put32(v,0x10300);Put32(v,0);Put32(v,1);Put32(v,0);
  return v;
}
guest::ShaderCapture Capture(std::vector<uint8_t> container,bool vertex) {
  guest::ShaderCapture c;c.container=std::move(container);c.vertex=vertex;c.hash=ContainerKey(c.container);return c;
}
std::vector<uint8_t> Container(uint8_t seed,size_t size=64) {
  std::vector<uint8_t> v(size);for(size_t i=0;i<size;++i) v[i]=uint8_t(seed+i*7);return v;
}
}
SR_TEST(shader_pack_round_trips_and_finds_by_container_size_and_stage) {
  auto vs=Container(1),ps=Container(2,96);
  auto file=BuildShaderPack({{ContainerKey(ps),uint32_t(ps.size()),ShaderStage::kPixel,Blob(ShaderStage::kPixel,5)},
                             {ContainerKey(vs),uint32_t(vs.size()),ShaderStage::kVertex,Blob(ShaderStage::kVertex,9)}});
  ShaderPack pack;std::string error;SR_CHECK(pack.Load(file,error));SR_CHECK_EQ(pack.size(),size_t(2));
  SR_CHECK(!pack.Find(ContainerKey(vs),uint32_t(vs.size()),ShaderStage::kVertex).empty());
  SR_CHECK(pack.Find(ContainerKey(vs),uint32_t(vs.size()),ShaderStage::kPixel).empty());
  SR_CHECK(pack.Find(ContainerKey(vs),uint32_t(vs.size())+4,ShaderStage::kVertex).empty());
  SR_CHECK(pack.Find(ContainerKey(Container(3)),64,ShaderStage::kVertex).empty());
}
SR_TEST(shader_pack_rejects_corruption_wrong_abi_and_truncation) {
  auto vs=Container(1);
  auto good=BuildShaderPack({{ContainerKey(vs),uint32_t(vs.size()),ShaderStage::kVertex,Blob(ShaderStage::kVertex,1)}});
  ShaderPack pack;std::string error;
  auto flipped=good;flipped.back()^=1;SR_CHECK(!pack.Load(flipped,error));SR_CHECK(error.find("integrity")!=std::string::npos);
  auto abi=good;abi[12]=2;SR_CHECK(!pack.Load(abi,error));SR_CHECK(error.find("ABI")!=std::string::npos);
  auto magic=good;magic[0]='X';SR_CHECK(!pack.Load(magic,error));
  auto cut=good;cut.resize(cut.size()-4);SR_CHECK(!pack.Load(cut,error));
  SR_CHECK(!pack.Load({},error));
  SR_CHECK(pack.Load(good,error));SR_CHECK_EQ(pack.size(),size_t(1));
  SR_CHECK(!pack.Load(flipped,error));SR_CHECK_EQ(pack.size(),size_t(0));  // a failed load leaves it empty
}
SR_TEST(shader_pack_lookup_reports_ready_unavailable_and_failed) {
  auto vs=Container(1),ps=Container(2),broken=Container(4),missing=Container(5);
  auto file=BuildShaderPack({{ContainerKey(vs),uint32_t(vs.size()),ShaderStage::kVertex,Blob(ShaderStage::kVertex,1)},
                             {ContainerKey(ps),uint32_t(ps.size()),ShaderStage::kPixel,Blob(ShaderStage::kPixel,2)},
                             {ContainerKey(broken),uint32_t(broken.size()),ShaderStage::kPixel,std::vector<uint8_t>(40,0xAB)}});
  auto pack=std::make_shared<ShaderPack>();std::string error;SR_CHECK(pack->Load(file,error));
  PackShaderLookup lookup(pack);
  auto ready=lookup(Capture(vs,true));SR_CHECK(ready.status==ShaderPoll::ready);
  SR_CHECK(ready.artifact && ready.artifact->words.size()==5 && ready.artifact->stage==ShaderStage::kVertex);
  SR_CHECK(lookup(Capture(ps,false)).status==ShaderPoll::ready);
  SR_CHECK(lookup(Capture(vs,false)).status==ShaderPoll::unavailable);   // the stage is part of the key
  SR_CHECK(lookup(Capture(missing,true)).status==ShaderPoll::unavailable);
  SR_CHECK(lookup(Capture(broken,false)).status==ShaderPoll::failed);
  SR_CHECK(lookup(Capture(missing,true)).status==ShaderPoll::unavailable);
  SR_CHECK_EQ(lookup.misses(),uint64_t(2));                               // one per distinct miss
}
