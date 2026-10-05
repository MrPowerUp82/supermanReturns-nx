#include "vulkan_shader_service.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
namespace superman_returns::graphics::shaders {
namespace {
class Reader {
  std::span<const uint8_t> bytes_;size_t position_=0;
public:
  explicit Reader(std::span<const uint8_t> b):bytes_(b) {}
  std::span<const uint8_t> Bytes(size_t n) {if(n>bytes_.size()-position_) throw std::runtime_error("Truncated shader result");auto r=bytes_.subspan(position_,n);position_+=n;return r;}
  uint32_t Word() {uint32_t n;auto b=Bytes(4);std::memcpy(&n,b.data(),4);return n;}
  std::string Text() {auto n=Word();if(n>16384) throw std::runtime_error("Oversized shader diagnostic/type");auto b=Bytes(n);return {reinterpret_cast<const char*>(b.data()),b.size()};}
  bool Done() const {return position_==bytes_.size();}
};
}
bool DecodeShaderResult(std::span<const uint8_t> bytes,ShaderStage stage,CompiledShader& result,std::string& error) {
  try {
    Reader r(bytes);if(r.Word()!=0x33525653 || r.Word()!=1) throw std::runtime_error("Shader result schema/ABI mismatch");
    uint32_t status=r.Word();if(r.Word()!=uint32_t(stage)) throw std::runtime_error("Shader result stage mismatch");
    if(status==0) throw std::runtime_error(r.Text());if(status!=1) throw std::runtime_error("Invalid shader status");
    CompiledShader out;out.stage=stage;auto& q=out.requirements;
    q.storage_buffers=r.Word();q.uniform_buffers=r.Word();q.sampled_images=r.Word();q.samplers=r.Word();q.descriptor_sets=r.Word();q.vertex_attributes=r.Word();q.vertex_output_components=r.Word();q.fragment_input_components=r.Word();
    auto feature=[&]() {auto v=r.Word();if(v>1) throw std::runtime_error("Invalid shader feature flag");return bool(v);};
    q.sampled_image_dynamic_indexing=feature();q.storage_buffer_dynamic_indexing=feature();q.clip_distance=feature();q.cull_distance=feature();
    for(auto* locations:{&out.inputs,&out.outputs}) {
      auto count=r.Word();if(count>256) throw std::runtime_error("Too many shader locations");
      for(uint32_t i=0;i<count;++i) {ShaderLocation l{r.Word(),r.Text()};if(l.location>255 || l.type.empty() || std::any_of(locations->begin(),locations->end(),[&](auto& v){return l.location==v.location;})) throw std::runtime_error("Invalid/duplicate shader location");locations->push_back(std::move(l));}
    }
    uint32_t size=r.Word();if(size<20 || size%4 || size>16*1024*1024) throw std::runtime_error("Empty/invalid compiled shader");
    auto data=r.Bytes(size);out.words.resize(size/4);std::memcpy(out.words.data(),data.data(),size);
    if(!r.Done() || out.words[0]!=0x07230203 || out.words[1]<0x10000 || out.words[1]>0x10300 || !out.words[3] || out.words[4]) throw std::runtime_error("Compiled shader header/trailing bytes invalid");
    result=std::move(out);error.clear();return true;
  } catch(const std::exception& e) {error=e.what();return false;}
}
std::vector<std::string> ValidateShaderPair(const CompiledShader& vs,const CompiledShader& ps) {
  std::vector<std::string> errors;if(vs.stage!=ShaderStage::kVertex || ps.stage!=ShaderStage::kPixel) errors.push_back("Shader pair stages invalid");
  for(auto& input:ps.inputs) if(std::none_of(vs.outputs.begin(),vs.outputs.end(),[&](auto& output){return input.location==output.location && input.type==output.type;})) errors.push_back("VS/PS location/type mismatch "+std::to_string(input.location));
  return errors;
}
} // namespace superman_returns::graphics::shaders
