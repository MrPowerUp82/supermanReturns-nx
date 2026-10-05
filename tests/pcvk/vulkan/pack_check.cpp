// pack_check <pack.srvk>: loads a shader pack the way the console does and decodes every
// result blob. Prints one line per shader; exit status 0 only if every entry decodes.
#include "pcvk/graphics/shaders/shader_pack.h"
#include <fstream>
#include <iostream>
#include <iterator>
using namespace superman_returns::graphics::shaders;
int main(int argc,char** argv) {
  if(argc!=2) {std::cerr<<"usage: pack_check <pack.srvk>\n";return 2;}
  std::ifstream in(argv[1],std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
  ShaderPack pack;std::string error;
  if(!pack.Load(std::move(bytes),error)) {std::cerr<<"load failed: "<<error<<"\n";return 1;}
  int failures=0;
  pack.ForEach([&](uint64_t key,uint32_t size,ShaderStage stage,std::span<const uint8_t> blob) {
    CompiledShader shader;std::string why;
    const bool ok=DecodeShaderResult(blob,stage,shader,why);
    std::cout<<(ok?"OK ":"BAD ")<<std::hex<<key<<std::dec<<' '<<(stage==ShaderStage::kVertex?"vs":"ps")<<" container="<<size
             <<" spirv_words="<<shader.words.size()<<" inputs="<<shader.inputs.size()<<" outputs="<<shader.outputs.size()
             <<(ok?"":" "+why)<<'\n';
    failures+=!ok;
  });
  std::cout<<pack.size()<<" entries, "<<failures<<" failed\n";
  return failures?1:0;
}
