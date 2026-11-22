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
// A display capture (nds::Display::frame_capture()) is made the same way,
// without the master brightness, as its recipe says (nds::CaptureRecipe):
// the main engine's picture with the scene or the scene alone, blended with
// a bank of video memory or with a capture made before, into a texture laid
// out as its bank. The last three are kept. A screen marked where it shows
// one is drawn with it put in, as with the scene.
//
// Made for speed, not to be exact: see nds/hardware_scene.h for what is
// left out. The program's own drawing (nds::PolygonRenderer) stays for
// where this cannot be used.
#pragma once

#include <SDL3/SDL.h>

#include <array>
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

  // The capture numbered `number`, made as `recipe` says, `scale` times as
  // large: its first source is `source` (256 * 192 colours with marks) with
  // `scene` put in, or `scene` alone. False if it could not be made.
  bool capture(const nds::HardwareScene& scene, const nds::CaptureRecipe& recipe,
               const std::vector<std::uint32_t>& source, std::uint64_t number, int scale);
  // A screen (`which`: 0 the top, 1 the bottom; `screen`: 256 * 192
  // colours with marks) with `scene` put in, or the kept capture numbered
  // `capture` if `scene` is null, `scale` times as large, and `brightness`
  // (the master brightness register) on what is put in. The texture to
  // show; null if it could not be made (`failed`) or that capture is not
  // kept.
  SDL_Texture* draw(int which, const std::vector<std::uint32_t>& screen, const nds::HardwareScene* scene,
                    std::uint64_t capture, std::uint16_t brightness, int scale, bool& failed);

 public:
  // The compose shader's uniforms, as std140 lays them out.
  struct ComposeUniforms {
    float brightness[4];  // 1 towards white, 2 towards black; how far (0 to 1).
    float options[4];     // 1 for a capture; the lines of the second texture (192, or 256 for a kept capture).
    std::uint32_t capture[4];  // How a capture is made (shaders/compose.frag).
    std::uint32_t edge[4];     // Edge marking: width in pixels, clear number, clear depth, 1 with the scene (+2 anti-aliased).
    float edge_colours[8][4];
    std::uint32_t fog[4];      // Fog: 1 with the scene (+2 alpha only), offset, step.
    float fog_colour[4];
    float clear_colour[4];     // The clear colour, as a pixel with no alpha holds it.
    std::uint32_t fog_table[8][4];
  };

 private:
  struct Place {
    std::uint16_t x = 0, y = 0;
  };

  struct Kept {
    SDL_GPUTexture* texture = nullptr;
    std::uint64_t number = 0;  // 0 for none.
  };

  bool ready(int scale);
  // Puts the scene (drawn first if it is new) or `source` into `screen`
  // where it is marked, into `target`; or makes a capture there, with
  // `second` as its second source (`bank` uploaded to bank_ first, if
  // given), as `uniforms` say.
  bool run(const nds::HardwareScene* scene, const std::vector<std::uint32_t>& screen, SDL_GPUTexture* source,
           SDL_GPUTexture* second, const std::vector<std::uint32_t>* bank, SDL_GPUTexture* target,
           const ComposeUniforms& uniforms);
  bool make_targets(int scale);
  void release_targets();
  // A place in the atlas for a texture, or false when it is full.
  bool allocate(int width, int height, Place& place);
  bool ensure_transfer(std::uint32_t bytes);
  bool ensure_vertex_buffer(std::uint32_t bytes);

  SDL_Renderer* renderer_ = nullptr;
  SDL_GPUDevice* device_ = nullptr;
  // By nds::HardwareScene::Kind, times two: nearer only, or as near too.
  SDL_GPUGraphicsPipeline* scene_pipelines_[16] = {};  // By kind, and depth equal.
  // The depth buffer has a stencil buffer, for polygon numbers and shadows
  // (without it, shadow polygons are left out).
  bool stencil_ = false;
  SDL_GPUGraphicsPipeline* compose_pipeline_ = nullptr;
  SDL_GPUSampler* nearest_ = nullptr;
  SDL_GPUTextureFormat depth_format_ = SDL_GPU_TEXTUREFORMAT_INVALID;
  SDL_GPUTexture* atlas_ = nullptr;
  SDL_GPUTexture* screen_ = nullptr;
  SDL_GPUTexture* bank_ = nullptr;  // A bank of video memory, 256 by 256, for a capture's second source.
  SDL_GPUTransferBuffer* transfer_ = nullptr;
  std::uint32_t transfer_size_ = 0;
  SDL_GPUBuffer* vertices_ = nullptr;
  std::uint32_t vertices_size_ = 0;

  // At the scale in use.
  int scale_ = 0;
  SDL_GPUTexture* scene_target_ = nullptr;
  SDL_GPUTexture* edges_ = nullptr;  // The scene's polygon numbers and depths, for edge marking.
  SDL_GPUTexture* fog_marks_ = nullptr;  // The scene's depths, and where fog is taken.
  SDL_GPUTexture* depth_ = nullptr;
  std::array<SDL_GPUTexture*, 2> outputs_{};  // For the top and the bottom screen,
  std::array<SDL_Texture*, 2> shown_{};       // as textures of the renderer.
  std::array<Kept, 3> kept_{};

  // The atlas: textures by key, placed on shelves from the top down.
  std::unordered_map<std::uint64_t, Place> placed_;
  int shelf_y_ = 0, shelf_x_ = 0, shelf_height_ = 0;
  bool atlas_full_noted_ = false;
  std::uint64_t scene_drawn_ = 0;  // The serial of the scene in scene_target_; 0 for none.
  std::vector<std::uint32_t> pending_;  // Indices of the scene's textures to upload.
};

}  // namespace bd::app
