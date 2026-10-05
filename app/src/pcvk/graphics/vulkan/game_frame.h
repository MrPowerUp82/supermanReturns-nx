#pragma once
#include "game_renderer.h"
#include <atomic>
#include <mutex>
#include <chrono>
#include <functional>
namespace superman_returns::graphics::vulkan {
// BasicLockable guarding vkQueueSubmit on the graphics queue. The PC host uses a plain mutex;
// on the Switch the queue belongs to the ReXGlue SDK device, whose own recursive mutex must be
// the one taken so the SDK presenter and the game renderer never submit concurrently.
class QueueLock {
public:
  QueueLock(std::mutex& m):lock_([&m]{m.lock();}),unlock_([&m]{m.unlock();}) {}
  QueueLock(std::function<void()> lock,std::function<void()> unlock):lock_(std::move(lock)),unlock_(std::move(unlock)) {}
  void lock() {lock_();}
  void unlock() {unlock_();}
private:
  std::function<void()> lock_,unlock_;
};
// Shares queue submission ordering with host presentation. Shader waits and
// CPU recording happen outside this mutex so the window can keep responding.
class GameFrame {
public:
  GameFrame(Context&,QueueLock,ShaderLookup,TextureDecoder);
  ~GameFrame();
  bool Initialize(const std::filesystem::path&,Error&);
  bool Enqueue(guest::RenderPacket&&,std::shared_ptr<TextureResource>&,Error&);
  void Cancel() {cancelled_.store(true);}
  std::function<void()> compilation_progress;
  GameRenderer& Renderer() {return renderer_;}
private:
  bool WaitShaders(Error&);
  bool WaitFence(Error&);
  Context& c_;QueueLock queue_mutex_;ShaderLookup shaders_;GameRenderer renderer_;
  std::vector<guest::RenderPacket> packets_;
  VkCommandPool pool_=VK_NULL_HANDLE;VkCommandBuffer command_=VK_NULL_HANDLE,upload_=VK_NULL_HANDLE;
  // Presentation snapshots, reused once neither a mailbox nor a submission holds them.
  std::vector<std::shared_ptr<TextureResource>> snapshots_;
  VkFence fence_=VK_NULL_HANDLE;uint64_t serial_=0;bool submitted_=false,failed_=false;
  std::atomic<bool> cancelled_{false};
  std::chrono::steady_clock::time_point cache_checkpoint_{};
};
}
