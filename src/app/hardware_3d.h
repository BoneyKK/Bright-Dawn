// The 3D scenes drawn by the graphics hardware, through SDL's GPU API on the
// window's own Vulkan device (the "gpu" renderer, main.cpp), at a whole
// number of times the DS's resolution.
//
// Each picture is made in two passes:
// 1. The scene (nds::HardwareScene): triangles with a depth buffer, their
//    textures read from one atlas that keeps each texture until it is full.
//    Drawn again only when the scene or the resolution changes.
// 2. The main engine's screen (256 by 192, with the marks of
//    nds::DisplayEngine::kMark3D) enlarged, with the scene put in where it
//    is marked, mixed as the marks say, and the master brightness on it.
// The result is a texture of the renderer, drawn in place of the main
// engine's screen.
//
// Made for speed, not to be exact: see nds/hardware_scene.h for what is
// left out. The program's own drawing (nds::PolygonRenderer) stays for
// where this cannot be used.
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "nds/hardware_scene.h"

namespace bd::app {

class Hardware3D {
 public:
  static constexpr int kLargestScale = 8;

  Hardware3D() = default;
  Hardware3D(const Hardware3D&) = delete;
  Hardware3D& operator=(const Hardware3D&) = delete;
  ~Hardware3D();

  // Makes what it needs on the renderer's GPU device. False, with the
  // reason, where the renderer has none or it cannot be used.
  bool open(SDL_Renderer* renderer, std::string& error);
  bool is_open() const { return device_ != nullptr; }

  // The main engine's screen (`screen`: 256 * 192 colours with marks) with
  // `scene` put in, `scale` times as large, and `brightness` (the master
  // brightness register) on the scene. The texture to show, or null if it
  // could not be made.
  SDL_Texture* draw(const nds::HardwareScene& scene, const std::vector<std::uint32_t>& screen,
                    std::uint16_t brightness, int scale);

 private:
  struct Place {
    std::uint16_t x = 0, y = 0;
  };

  bool make_targets(int scale);
  void release_targets();
  // A place in the atlas for a texture, or false when it is full.
  bool allocate(int width, int height, Place& place);
  bool ensure_transfer(std::uint32_t bytes);
  bool ensure_vertex_buffer(std::uint32_t bytes);

  SDL_Renderer* renderer_ = nullptr;
  SDL_GPUDevice* device_ = nullptr;
  // By nds::HardwareScene::Kind, times two: nearer only, or as near too.
  SDL_GPUGraphicsPipeline* scene_pipelines_[6] = {};
  bool stencil_ = false;  // The depth buffer has a stencil buffer, for polygon numbers.
  SDL_GPUGraphicsPipeline* compose_pipeline_ = nullptr;
  SDL_GPUSampler* nearest_ = nullptr;
  SDL_GPUTextureFormat depth_format_ = SDL_GPU_TEXTUREFORMAT_INVALID;
  SDL_GPUTexture* atlas_ = nullptr;
  SDL_GPUTexture* screen_ = nullptr;
  SDL_GPUTransferBuffer* transfer_ = nullptr;
  std::uint32_t transfer_size_ = 0;
  SDL_GPUBuffer* vertices_ = nullptr;
  std::uint32_t vertices_size_ = 0;

  // At the scale in use.
  int scale_ = 0;
  SDL_GPUTexture* scene_target_ = nullptr;
  SDL_GPUTexture* depth_ = nullptr;
  SDL_GPUTexture* output_ = nullptr;
  SDL_Texture* shown_ = nullptr;

  // The atlas: textures by key, placed on shelves from the top down.
  std::unordered_map<std::uint64_t, Place> placed_;
  int shelf_y_ = 0, shelf_x_ = 0, shelf_height_ = 0;
  bool atlas_full_noted_ = false;
  std::uint64_t scene_drawn_ = 0;  // The serial of the scene in scene_target_; 0 for none.
  std::vector<std::uint32_t> pending_;  // Indices of the scene's textures to upload.
};

}  // namespace bd::app
