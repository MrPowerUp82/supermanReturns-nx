#include "shader_pack.h"
#include <algorithm>
#include <cstring>
namespace superman_returns::graphics::shaders {
namespace {
constexpr size_t kHeader = 32, kEntry = 32;
uint32_t Rd32(const uint8_t* p) {uint32_t v;std::memcpy(&v,p,4);return v;}
uint64_t Rd64(const uint8_t* p) {uint64_t v;std::memcpy(&v,p,8);return v;}
void Wr32(std::vector<uint8_t>& out,size_t at,uint32_t v) {std::memcpy(out.data()+at,&v,4);}
void Wr64(std::vector<uint8_t>& out,size_t at,uint64_t v) {std::memcpy(out.data()+at,&v,8);}
}
uint64_t Fnv1a64(std::span<const uint8_t> bytes,uint64_t seed) {
  uint64_t h=seed;for(auto b:bytes) {h^=b;h*=1099511628211ull;}return h;
}
uint64_t ContainerKey(std::span<const uint8_t> container) {return Fnv1a64(container);}
std::vector<uint8_t> BuildShaderPack(std::vector<PackInput> inputs) {
  std::sort(inputs.begin(),inputs.end(),[](const PackInput& a,const PackInput& b){return a.key!=b.key?a.key<b.key:a.stage<b.stage;});
  size_t total=kHeader+inputs.size()*kEntry;
  for(auto& i:inputs) total+=i.blob.size();
  std::vector<uint8_t> out(total);
  std::memcpy(out.data(),kShaderPackMagic,8);
  Wr32(out,8,kShaderPackVersion);Wr32(out,12,BindingContractVersion);Wr32(out,16,uint32_t(inputs.size()));
  size_t table=kHeader,blob=kHeader+inputs.size()*kEntry;
  for(auto& i:inputs) {
    Wr64(out,table,i.key);Wr32(out,table+8,i.container_size);Wr32(out,table+12,i.stage==ShaderStage::kVertex?0:1);
    Wr64(out,table+16,blob);Wr32(out,table+24,uint32_t(i.blob.size()));
    std::memcpy(out.data()+blob,i.blob.data(),i.blob.size());blob+=i.blob.size();table+=kEntry;
  }
  Wr64(out,24,Fnv1a64({out.data()+kHeader,out.size()-kHeader}));
  return out;
}
bool ShaderPack::Load(std::vector<uint8_t> file,std::string& error) {
  entries_.clear();file_.clear();
  auto fail=[&](const char* text) {error=text;return false;};
  if(file.size()<kHeader || std::memcmp(file.data(),kShaderPackMagic,8)) return fail("Not a Vulkan shader pack");
  if(Rd32(file.data()+8)!=kShaderPackVersion) return fail("Unsupported shader pack version");
  if(Rd32(file.data()+12)!=BindingContractVersion) return fail("Shader pack was built for another binding ABI");
  const uint32_t count=Rd32(file.data()+16);
  if(Rd32(file.data()+20)) return fail("Reserved shader pack field is not zero");
  if(count>1u<<20 || uint64_t(count)*kEntry>file.size()-kHeader) return fail("Shader pack entry table exceeds the file");
  if(Rd64(file.data()+24)!=Fnv1a64({file.data()+kHeader,file.size()-kHeader})) return fail("Shader pack failed its integrity check");
  std::vector<Entry> entries;entries.reserve(count);
  const uint64_t blobs=kHeader+uint64_t(count)*kEntry;uint64_t expected=blobs;
  for(uint32_t i=0;i<count;++i) {
    const uint8_t* e=file.data()+kHeader+size_t(i)*kEntry;
    Entry entry{Rd64(e),Rd32(e+8),Rd32(e+12),Rd64(e+16),Rd32(e+24)};
    if(entry.stage>1 || Rd32(e+28)) return fail("Invalid shader pack entry");
    // Blobs are stored back to back in table order: any gap or overlap means corruption.
    if(entry.offset!=expected || entry.length<20 || entry.offset+entry.length>file.size()) return fail("Shader pack blob is out of bounds");
    expected+=entry.length;
    if(!entries.empty()) {
      const Entry& p=entries.back();
      if(p.key>entry.key || (p.key==entry.key && p.stage>=entry.stage)) return fail("Shader pack entries are not sorted/unique");
    }
    entries.push_back(entry);
  }
  if(expected!=file.size()) return fail("Shader pack has trailing bytes");
  file_=std::move(file);entries_=std::move(entries);error.clear();return true;
}
std::span<const uint8_t> ShaderPack::Find(uint64_t key,uint32_t container_size,ShaderStage stage) const {
  const uint32_t s=stage==ShaderStage::kVertex?0:1;
  auto it=std::lower_bound(entries_.begin(),entries_.end(),std::pair{key,s},[](const Entry& e,const std::pair<uint64_t,uint32_t>& k){return e.key!=k.first?e.key<k.first:e.stage<k.second;});
  if(it==entries_.end() || it->key!=key || it->stage!=s || it->container_size!=container_size) return {};
  return {file_.data()+it->offset,it->length};
}
void ShaderPack::ForEach(const Visitor& visit) const {
  for(const auto& e:entries_) visit(e.key,e.container_size,e.stage?ShaderStage::kPixel:ShaderStage::kVertex,{file_.data()+e.offset,e.length});
}
ShaderResult PackShaderLookup::operator()(const guest::ShaderCapture& capture) const {
  const uint64_t key=ContainerKey(capture.container);
  const uint32_t stage=capture.vertex?0:1;
  std::lock_guard lock(mutex_);
  auto cached=cache_.find({key,stage});
  if(cached!=cache_.end()) return cached->second;
  ShaderResult result;
  auto blob=pack_?pack_->Find(key,uint32_t(capture.container.size()),capture.vertex?ShaderStage::kVertex:ShaderStage::kPixel):std::span<const uint8_t>{};
  if(blob.empty()) {
    result.status=ShaderPoll::unavailable;++misses_;
    result.diagnostic="Shader container "+std::to_string(key)+" is not in the Vulkan shader pack";
  } else {
    auto artifact=std::make_shared<CompiledShader>();
    if(DecodeShaderResult(blob,capture.vertex?ShaderStage::kVertex:ShaderStage::kPixel,*artifact,result.diagnostic)) {
      result.status=ShaderPoll::ready;result.artifact=std::move(artifact);
    } else result.status=ShaderPoll::failed;
  }
  cache_.emplace(std::pair{key,stage},result);
  return result;
}
} // namespace superman_returns::graphics::shaders
