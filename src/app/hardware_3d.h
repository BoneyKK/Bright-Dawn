// The 3D scenes drawn by the graphics hardware, through SDL's GPU API on the
// window's own Vulkan device (the "gpu" renderer, main.cpp), at a whole
// number of times the DS's resolution.
//
// Each picture is made in two passes:
// 1. The scene (nds::HardwareScene): triangles with a depth buffer, their
//    textures read from one atlas that keeps each texture until it is full.
//    Drawn again only when the scene or the resolution changes. Where the
//    frame asks for outlines or anti-aliasing, the scene's polygons are also
//    drawn as the DS draws them, by two compute shaders (shaders/raster.comp
//    and finish.comp: the processor's rules, two layers a pixel), and those
//    pixels are taken from there.
// 2. The main engine's screen (256 by 192, with the marks of
//    nds::DisplayEngine::kMark3D) enlarged, with the scene put in where it
//    is marked, mixed as the marks say, and the master brightness on it.
// The result is a texture of the renderer, drawn in place of the main
// engine's screen.
//
// The 2D picture of a screen is the processor's, uploaded, or is drawn
// here too, before step 2, from the lines the processor recorded
// (nds/frame_2d.h) by shaders/engine2d.frag, which draws each pixel as
// nds::DisplayEngine does. A screen with nothing put in is then step 2
// alone, without the marks.
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

#include "nds/frame_2d.h"
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
  // A screen's 2D picture: the processor's (`pixels`: 256 * 192 colours
  // with marks), or the lines the graphics hardware draws it from.
  struct Picture {
    const std::vector<std::uint32_t>* pixels = nullptr;
    const nds::Frame2D* lines = nullptr;
    const nds::Video2D* video = nullptr;
  };
  // A screen (`which`: 0 the top, 1 the bottom) with `scene` put in, or
  // the kept capture numbered `capture` if `scene` is null, or (both null
  // and 0) nothing put in, `scale` times as large, and `brightness` (the
  // master brightness register) on what is put in. The texture to show;
  // null if it could not be made (`failed`) or that capture is not kept.
  SDL_Texture* draw(int which, const Picture& picture, const nds::HardwareScene* scene, std::uint64_t capture,
                    std::uint16_t brightness, int scale, bool& failed);
  // The same with the processor's picture.
  SDL_Texture* draw(int which, const std::vector<std::uint32_t>& screen, const nds::HardwareScene* scene,
                    std::uint64_t capture, std::uint16_t brightness, int scale, bool& failed) {
    Picture picture;
    picture.pixels = &screen;
    return draw(which, picture, scene, capture, brightness, scale, failed);
  }
  // For tests and checks: the last picture draw() made of screen `which`,
  // as red, green, blue and alpha bytes (red lowest), row after row, 256 *
  // scale by 192 * scale. False if it cannot be read.
  bool read_screen(int which, std::vector<std::uint32_t>& pixels);
  // For tests and checks: the last 2D picture of a screen, drawn or
  // uploaded, as red, green, blue and the mark (bits 24 to 31 of nds::
  // DisplayEngine's colours) bytes, red lowest, 256 by 192.
  bool read_2d(std::vector<std::uint32_t>& pixels);
  // The captures kept are no longer the console's (it was started anew, and
  // numbers its captures from 1 again).
  void forget_captures() {
    for (Kept& k : kept_) k.number = 0;
  }
  // For tests and checks: the last scene as the compute shaders drew it, as
  // the processor would (six bits each of red, green, blue and alpha, plus
  // 1 << 24 where the screen takes it), row after row at the scale drawn.
  // False if the last scene was not drawn so.
  bool read_raster(std::vector<std::uint32_t>& pixels);

 public:
  // The compose shader's uniforms, as std140 lays them out.
  struct ComposeUniforms {
    float brightness[4];  // 1 towards white, 2 towards black; how far (0 to 1).
    float options[4];     // 1 for a capture; the lines of the second texture (192, or 256 for a kept capture).
    std::uint32_t capture[4];  // How a capture is made (shaders/compose.frag).
    std::uint32_t edge[4];     // The rasterizer's pixels: width, height, 0, 1 where they are the scene's.
    std::uint32_t fog[4];      // Fog: 1 with the scene (+2 alpha only), offset, step.
    float fog_colour[4];
    float clear_colour[4];     // The clear colour, as a pixel with no alpha holds it.
    std::uint32_t fog_table[8][4];
    // Outlines from the scene's polygon numbers and depths (above the DS's
    // resolution): width (0 for none), the clear plane's number and depth,
    // the scale.
    std::uint32_t outline[4];
    float outline_colours[8][4];
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
  // Without `marks`, the 2D picture's marks are left out (nothing is put
  // in).
  bool run(const nds::HardwareScene* scene, const Picture& picture, SDL_GPUTexture* source, SDL_GPUTexture* second,
           const std::vector<std::uint32_t>* bank, SDL_GPUTexture* target, const ComposeUniforms& uniforms,
           bool marks = true);
  // Draws the scene's polygons as the DS does (shaders/raster.comp), then
  // finishes them (shaders/finish.comp), into finished_.
  void rasterize_scene(SDL_GPUCommandBuffer* commands, const nds::HardwareScene& scene);
  bool make_targets(int scale);
  void release_targets();
  // A place in the atlas for a texture, or false when it is full.
  bool allocate(int width, int height, Place& place);
  bool ensure_transfer(std::uint32_t bytes);
  bool ensure_vertex_buffer(std::uint32_t bytes);
  // A storage buffer of at least `bytes`: for the rasterizer, or (`graphics`)
  // for a fragment shader.
  bool ensure_storage(SDL_GPUBuffer*& buffer, std::uint32_t& size, std::uint32_t bytes, bool graphics = false);
  // Copies `bytes` from `texture` (its whole, `width` by `height`) or from
  // `buffer` into `out`, waiting for the graphics hardware.
  bool download(SDL_GPUTexture* texture, int width, int height, SDL_GPUBuffer* buffer, std::uint32_t bytes, void* out);

  SDL_Renderer* renderer_ = nullptr;
  SDL_GPUDevice* device_ = nullptr;
  // By nds::HardwareScene::Kind, times two: nearer only, or as near too.
  SDL_GPUGraphicsPipeline* scene_pipelines_[16] = {};  // By kind, and depth equal.
  // The depth buffer has a stencil buffer, for polygon numbers and shadows
  // (without it, shadow polygons are left out).
  bool stencil_ = false;
  SDL_GPUGraphicsPipeline* compose_pipeline_ = nullptr;
  SDL_GPUComputePipeline* raster_pipeline_ = nullptr;  // shaders/raster.comp
  SDL_GPUComputePipeline* finish_pipeline_ = nullptr;  // shaders/finish.comp
  SDL_GPUGraphicsPipeline* engine2d_pipeline_ = nullptr;  // shaders/engine2d.frag
  SDL_GPUSampler* nearest_ = nullptr;
  SDL_GPUTextureFormat depth_format_ = SDL_GPU_TEXTUREFORMAT_INVALID;
  SDL_GPUTexture* atlas_ = nullptr;
  SDL_GPUTexture* screen_ = nullptr;
  SDL_GPUTexture* bank_ = nullptr;  // A bank of video memory, 256 by 256, for a capture's second source.
  SDL_GPUTransferBuffer* transfer_ = nullptr;
  std::uint32_t transfer_size_ = 0;
  SDL_GPUBuffer* vertices_ = nullptr;
  std::uint32_t vertices_size_ = 0;
  // The rasterizer's corners and polygons (nds::HardwareScene::Raster).
  SDL_GPUBuffer* corners_ = nullptr;
  std::uint32_t corners_size_ = 0;
  SDL_GPUBuffer* polygons_ = nullptr;
  std::uint32_t polygons_size_ = 0;
  // For the 2D pictures: video memory, where the engines' pages are in it,
  // and an engine's lines, palette copies and sprite attribute copies.
  SDL_GPUBuffer* video_ = nullptr;
  std::uint32_t video_size_ = 0;
  std::vector<std::uint32_t> video_versions_;  // nds::Video2D::versions of what video_ holds.
  SDL_GPUBuffer* layout_ = nullptr;
  std::uint32_t layout_size_ = 0;
  SDL_GPUBuffer* lines_ = nullptr;
  std::uint32_t lines_size_ = 0;
  SDL_GPUBuffer* palettes_ = nullptr;
  std::uint32_t palettes_size_ = 0;
  SDL_GPUBuffer* sprite_copies_ = nullptr;
  std::uint32_t sprite_copies_size_ = 0;
  SDL_GPUBuffer* changes_ = nullptr;  // nds::Video2D::changes, then nds::Video2D::changed.
  std::uint32_t changes_size_ = 0;

  // At the scale in use.
  int scale_ = 0;
  SDL_GPUTexture* scene_target_ = nullptr;
  SDL_GPUTexture* fog_marks_ = nullptr;  // The scene's depths, and where fog is taken.
  SDL_GPUTexture* numbers_ = nullptr;    // The numbers of the opaque polygons drawn (scene.frag).
  SDL_GPUBuffer* layers_ = nullptr;      // The rasterizer's two layers, 16 bytes a pixel.
  SDL_GPUBuffer* finished_ = nullptr;    // And its pixels when finished, 4 bytes each.
  bool rasterized_ = false;              // The scene in scene_target_ is in finished_ too.
  SDL_GPUTexture* depth_ = nullptr;
  std::array<SDL_GPUTexture*, 2> outputs_{};  // For the top and the bottom screen,
  std::array<SDL_Texture*, 2> shown_{};       // as textures of the renderer.
  std::array<Kept, 3> kept_{};

  // The atlas: textures by key, placed on shelves from the top down.
  std::unordered_map<std::uint64_t, Place> placed_;
  int shelf_y_ = 0, shelf_x_ = 0, shelf_height_ = 0;
  bool atlas_full_noted_ = false;
  int log_compose_column_ = 0;  // The frame log's column for the next compose.
  std::uint64_t scene_drawn_ = 0;  // The serial of the scene in scene_target_; 0 for none.
  std::vector<std::uint32_t> pending_;  // Indices of the scene's textures to upload.
};

}  // namespace bd::app
