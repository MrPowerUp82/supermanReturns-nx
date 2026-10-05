#include "vulkan_shader_service.h"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
namespace superman_returns::graphics::shaders {
namespace {
std::vector<uint8_t> ReadFile(const std::filesystem::path& p) {
  std::ifstream f(p,std::ios::binary|std::ios::ate);auto size=f.tellg();
  if(!f || size<0 || size>17*1024*1024) throw std::runtime_error("Missing/oversized shader result");
  std::vector<uint8_t> bytes(static_cast<size_t>(size));f.seekg(0);if(!f.read(reinterpret_cast<char*>(bytes.data()),size)) throw std::runtime_error("Shader result read failed");return bytes;
}
}
struct VulkanShaderService::Impl {
  struct Job {ShaderStage stage;std::vector<uint8_t> container;ShaderResult result;};
  VulkanShaderConfig config;ShaderProcess process;mutable std::mutex mutex;std::condition_variable cv;std::map<ShaderKey,Job> jobs;std::deque<ShaderKey> queue;bool stop=false;std::vector<std::jthread> threads;
  Impl(VulkanShaderConfig c,ShaderProcess p):config(std::move(c)),process(std::move(p)) {
    for(uint32_t i=0;i<std::clamp(config.compiler_workers,1u,4u);++i) threads.emplace_back([this](std::stop_token token){Run(token);});
  }
  ~Impl() {{std::lock_guard lock(mutex);stop=true;}for(auto& thread:threads) thread.request_stop();cv.notify_all();for(auto& thread:threads) thread.join();}
  void Run(std::stop_token token) {
    std::stop_callback wake(token,[this] {cv.notify_all();});
    for(;;) {
      ShaderKey key;ShaderStage stage;std::vector<uint8_t> container;
      {std::unique_lock lock(mutex);cv.wait(lock,[&]{return stop || token.stop_requested() || !queue.empty();});if(stop || token.stop_requested()) return;key=queue.front();queue.pop_front();auto& j=jobs.at(key);stage=j.stage;container=j.container;}
      ShaderResult result;result.status=ShaderPoll::failed;
      try {
        auto directory=config.cache/"runtime_requests"/std::to_string(key);std::filesystem::create_directories(directory);
        auto raw=directory/(stage==ShaderStage::kVertex?"shader.vs.bin":"shader.ps.bin"),output=directory/"result.bin";
        {std::ofstream file(raw,std::ios::binary|std::ios::trunc);if(!file.write(reinterpret_cast<const char*>(container.data()),container.size())) throw std::runtime_error("Cannot write captured shader container");}
        std::error_code ignored;std::filesystem::remove(output,ignored);
        std::vector<std::filesystem::path> args{config.python,config.script,"--container",raw,"--stage",stage==ShaderStage::kVertex?"vs":"ps","--cache",config.cache,"--emitter",config.emitter,"--common",config.common,"--dxc",config.dxc,"--result",output};
        if(!process || !process(args,config.timeout,token,result.diagnostic)) {if(result.diagnostic.empty()) result.diagnostic="Shader compiler process failed";}
        else {auto artifact=std::make_shared<CompiledShader>();if(DecodeShaderResult(ReadFile(output),stage,*artifact,result.diagnostic)) {result.status=ShaderPoll::ready;result.artifact=std::move(artifact);}}
      } catch(const std::exception& e) {result.diagnostic=e.what();}
      {std::lock_guard lock(mutex);jobs.at(key).result=std::move(result);}
    }
  }
};
VulkanShaderService::VulkanShaderService(VulkanShaderConfig c,ShaderProcess p):impl_(std::make_unique<Impl>(std::move(c),std::move(p))) {}
VulkanShaderService::~VulkanShaderService()=default;
ShaderKey VulkanShaderService::Request(std::span<const uint8_t> bytes,ShaderStage stage) {
  if(bytes.empty() || bytes.size()>16*1024*1024) return 0;
  uint64_t hash=14695981039346656037ull;for(auto b:bytes) {hash^=b;hash*=1099511628211ull;}hash^=uint32_t(stage);hash*=1099511628211ull;if(!hash) hash=1;
  std::lock_guard lock(impl_->mutex);
  // A hash collision never aliases a different container or stage.
  for(;;) {
    auto found=impl_->jobs.find(hash);if(found==impl_->jobs.end()) break;
    if(found->second.stage==stage && std::equal(bytes.begin(),bytes.end(),found->second.container.begin(),found->second.container.end())) return hash;
    if(!++hash) hash=1;
  }
  impl_->jobs.emplace(hash,Impl::Job{stage,{bytes.begin(),bytes.end()},{}});impl_->queue.push_back(hash);impl_->cv.notify_one();return hash;
}
ShaderResult VulkanShaderService::Poll(ShaderKey key) const {
  std::lock_guard lock(impl_->mutex);auto found=impl_->jobs.find(key);if(found==impl_->jobs.end()) return {ShaderPoll::failed,{},"Unknown/empty captured shader"};return found->second.result;
}
} // namespace superman_returns::graphics::shaders
