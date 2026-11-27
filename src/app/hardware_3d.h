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
// out as its bank. Four are kept, those the console keeps among them. A
// screen marked where it shows one (nds::DisplayEngine::kCapturedBelow) is
// drawn with it put in at its resolution, under the scene, as each line's
// map says (nds::CaptureLines): shown as its bank is, or as a bitmap
// background turned and made larger or smaller. A capture's first source
// can have one put in too.
//
// Widescreen (set_wide): the scene, from the geometry engine's wide frame
// (nds::HardwareScene::wide), is drawn a third wider than the DS's screen
// (picture_width()), the DS's 256 columns in the middle; a screen is that
// wide too, its 2D picture in the middle. At its sides: where the processor
// draws the 2D, its side columns (Picture::sides) over the scene; else the
// scene alone (with fog, outlines, the 3D's effect at the line's end and the
// master brightness) on the lines that show it, else black. A kept capture is as
// wide too: one of whole lines has sides as a screen's, and a screen that
// shows one at a line's end shows its sides there. The rasterizer (1x) is
// not used then.
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

  // Widescreen (see above). Changing it lets go of the pictures made and
  // the captures kept.
  void set_wide(bool on) {
    if (on == wide_) return;
    wide_ = on;
    if (device_ != nullptr) release_targets();
  }
  bool wide() const { return wide_; }
  // How many pixels across the scene and the screens are at `scale`: 256
  // times it, or (wide) an even number near a third more.
  static int picture_width(int scale, bool wide) {
    return wide ? 2 * ((1024 * scale + 3) / 6) : 256 * scale;
  }
  // A wide picture's side at `scale`, in whole DS pixels (rounded down): how
  // far the widescreen HUD moves an element (nds::WideHud): 43 at 1x and 2x,
  // 42 at 3x and 4x.
  static int side_shift(int scale) {
    scale = scale < 1 ? 1 : scale;
    return (picture_width(scale, true) - 256 * scale) / (2 * scale);
  }

  // The capture numbered `number`, made as `recipe` says, `scale` times as
  // large: its first source is `source` (256 * 192 colours with marks) with
  // `scene` put in, or `scene` alone. It takes the place of a capture not
  // in `keep` (those the console keeps, nds::Display::kept_numbers()), if
  // there is one. False if it could not be made.
  // `sides`: the source's sides (nds::Display::capture_sides()), or null.
  bool capture(const nds::HardwareScene& scene, const nds::CaptureRecipe& recipe,
               const std::vector<std::uint32_t>& source, std::uint64_t number, int scale,
               const std::array<std::uint64_t, 3>& keep = {}, const std::vector<std::uint32_t>* sides = nullptr);
  // A screen's 2D picture: the processor's (`pixels`: 256 * 192 colours
  // with marks), or the lines the graphics hardware draws it from.
  struct Picture {
    const std::vector<std::uint32_t>* pixels = nullptr;
    const nds::Frame2D* lines = nullptr;
    const nds::Video2D* video = nullptr;
    // Widescreen, with `pixels`: the 2D picture at its sides (nds::Display::
    // screen_sides()), or null.
    const std::vector<std::uint32_t>* sides = nullptr;
  };
  // A screen (`which`: 0 the top, 1 the bottom) with `scene` (if any) and
  // the kept capture numbered `capture` (if not 0) put in, `scale` times as
  // large, and `brightness` (the master brightness register) on what is put
  // in. The capture goes under the scene, where the screen is marked nds::
  // DisplayEngine::kCapturedBelow, as `lines` map it. With neither, nothing
  // is put in. The texture to show; null if it could not be made (`failed`)
  // or, without the scene, that capture is not kept.
  SDL_Texture* draw(int which, const Picture& picture, const nds::HardwareScene* scene, std::uint64_t capture,
                    const nds::CaptureLines* lines, std::uint16_t brightness, int scale, bool& failed);
  // The same with the processor's picture.
  SDL_Texture* draw(int which, const std::vector<std::uint32_t>& screen, const nds::HardwareScene* scene,
                    std::uint64_t capture, const nds::CaptureLines* lines, std::uint16_t brightness, int scale,
                    bool& failed) {
    Picture picture;
    picture.pixels = &screen;
    return draw(which, picture, scene, capture, lines, brightness, scale, failed);
  }
  // The last picture draw() made of screen `which`, its columns from
  // `from_x` (`from_w` of them) and all its lines, made `width` by `height`
  // pixels by averaging the part of it each covers (shaders/resolve.frag):
  // for a window that shows it smaller than it is made. Null if it cannot
  // be made.
  SDL_Texture* downscaled(int which, float from_x, float from_w, int width, int height);
  // For tests and checks: the last picture downscaled() made of screen
  // `which`, as read_screen() gives it, `width` by `height`.
  bool read_downscaled(int which, std::vector<std::uint32_t>& pixels);
  // For tests and checks: the last picture draw() made of screen `which`,
  // as red, green, blue and alpha bytes (red lowest), row after row,
  // picture_width() by 192 * scale. False if it cannot be read.
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
  // The pixels the opaque polygons of `scene` (those that keep a polygon
  // number) can cover in a picture `width` by `height`, a pixel more on
  // each side: left, top, right and bottom, the right and bottom ones past
  // the last. All 0 if there are none. Outlines are looked for only there.
  // `across`: x is drawn times it (a wide picture's, 256 * scale / width).
  static std::array<std::uint32_t, 4> opaque_bounds(const nds::HardwareScene& scene, std::uint32_t width,
                                                    std::uint32_t height, float across = 1.0f);
  // Where outlines are looked for at `scale`, where the DS's pixels
  // `shown` (left, top, right and bottom, as above) show the scene: the
  // opaque polygons' bounds, within a DS pixel of those. In a wide picture
  // (`wide`) the sides show the scene on the lines it is shown on.
  static std::array<std::uint32_t, 4> outline_area(const nds::HardwareScene& scene,
                                                   const std::array<std::uint32_t, 4>& shown, int scale,
                                                   bool wide = false);

 public:
  // The compose shader's uniforms, as std140 lays them out.
  struct ComposeUniforms {
    float brightness[4];  // 1 towards white, 2 towards black; how far (0 to 1).
    float options[4];  // 1 for a capture.
    std::uint32_t capture[4];  // How a capture is made (shaders/compose.frag).
    std::uint32_t edge[4];     // The rasterizer's pixels: width, height, 0, 1 where they are the scene's.
    std::uint32_t fog[4];      // Fog: 1 with the scene (+2 alpha only), offset, step.
    float fog_colour[4];
    float clear_colour[4];     // The clear colour, as a pixel with no alpha holds it.
    std::uint32_t fog_table[8][4];
    // Outlines from the scene's polygon numbers and depths (above the DS's
    // resolution, shaders/outline.frag): 1 where they are drawn.
    std::uint32_t outline[4];
    std::uint32_t outline_bounds[4];  // Where outlines can be: opaque_bounds().
    // Widescreen: 1; where the DS's screen starts and how much of the width
    // it is (0 to 1 each); 1 where the sides show the scene.
    float wide[4];
    std::uint32_t wide_rows[8];  // The lines whose sides show the scene: a bit each, from bit 0 of the first.
    // 1 where a kept capture is put in; 1 where the second source is a kept
    // capture; 1 where there is no scene; 1 where the sides are given.
    std::uint32_t put_info[4];
    // 1 where the kept capture is put in at the sides only; the blur there
    // (CaptureLines::blur); the difference added to each part there
    // (CaptureLines::offset, plus 128, a byte each from red).
    std::uint32_t put_sides[4];
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
  // The kept capture numbered `number`; null for none.
  const Kept* kept(std::uint64_t number) const;
  // `lines` into put_map_ (each line's map of the capture put in, for the
  // compose shader: x0, y0, pa, pc; then pb, pd, the bank's row, and 1 if
  // shown + 2 if it repeats + its rows times 4).
  void put_lines(const nds::CaptureLines& lines);
  // Puts the scene (drawn first if it is new) and `put` (a kept capture)
  // into `screen` where it is marked, into `target`; or makes a capture
  // there, with `second` as its second source (`bank` uploaded to bank_
  // first, if given), as `uniforms` say.
  // Without `marks`, the 2D picture's marks are left out (nothing is put
  // in).
  bool run(const nds::HardwareScene* scene, const Picture& picture, SDL_GPUTexture* put, SDL_GPUTexture* second,
           const std::vector<std::uint32_t>* bank, SDL_GPUTexture* target, const ComposeUniforms& uniforms,
           bool marks = true);
  // Draws the scene's polygons as the DS does (shaders/raster.comp), then
  // finishes them (shaders/finish.comp), into finished_.
  void rasterize_scene(SDL_GPUCommandBuffer* commands, const nds::HardwareScene& scene);
  // Above 1x: the edges of the scene's opaque polygons within `area`
  // (outline_area()), and the outlines drawn from them into outlines_, for
  // the compose pass to put in.
  void find_edges(SDL_GPUCommandBuffer* commands, const nds::HardwareScene& scene,
                  const std::array<std::uint32_t, 4>& area);
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
  SDL_GPUGraphicsPipeline* edges_pipeline_ = nullptr;     // shaders/edges.frag
  SDL_GPUGraphicsPipeline* nearby_pipeline_ = nullptr;    // shaders/nearby.frag
  SDL_GPUGraphicsPipeline* outline_pipeline_ = nullptr;   // shaders/outline.frag
  SDL_GPUGraphicsPipeline* resolve_pipeline_ = nullptr;   // shaders/resolve.frag
  SDL_GPUSampler* nearest_ = nullptr;
  SDL_GPUTextureFormat depth_format_ = SDL_GPU_TEXTUREFORMAT_INVALID;
  SDL_GPUTexture* atlas_ = nullptr;
  SDL_GPUTexture* screen_ = nullptr;
  SDL_GPUTexture* bank_ = nullptr;  // A bank of video memory, 256 by 256, for a capture's second source.
  SDL_GPUTexture* sides_ = nullptr;  // Picture::sides, as screen_ holds the screen's pixels.
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
  std::array<float, 192 * 8> put_map_{};  // put_lines()'s, for the next run(),
  SDL_GPUBuffer* put_map_buffer_ = nullptr;  // and where the shader reads it.
  std::uint32_t put_map_size_ = 0;

  bool wide_ = false;
  // At the scale in use, and how wide the pictures are (picture_width()).
  int scale_ = 0;
  int width_ = 0;
  SDL_GPUTexture* scene_target_ = nullptr;
  SDL_GPUTexture* fog_marks_ = nullptr;  // The scene's depths, and where fog is taken.
  SDL_GPUTexture* numbers_ = nullptr;    // The numbers of the opaque polygons drawn (scene.frag).
  // Above 1x, for outlines: the edges of the opaque polygons, a bit for
  // each pixel, 16 across to each of its own (shaders/edges.frag).
  SDL_GPUTexture* edges_ = nullptr;
  SDL_GPUTexture* nearby_ = nullptr;  // Where edges are near: a pixel for each 16 by 16 (shaders/nearby.frag).
  // The outlines, as wide as asked, in the edge colours: once for each scene
  // (shaders/outline.frag), and put into each picture of it by compose.frag.
  SDL_GPUTexture* outlines_ = nullptr;
  bool outlined_ = false;  // edges_ and outlines_ are for the scene in scene_target_,
  std::array<std::uint32_t, 4> outline_bounds_{};  // where it is (all 0 for nowhere).
  SDL_GPUBuffer* layers_ = nullptr;      // The rasterizer's two layers, 16 bytes a pixel.
  SDL_GPUBuffer* finished_ = nullptr;    // And its pixels when finished, 4 bytes each.
  bool rasterized_ = false;              // The scene in scene_target_ is in finished_ too.
  SDL_GPUTexture* depth_ = nullptr;
  std::array<SDL_GPUTexture*, 2> outputs_{};  // For the top and the bottom screen,
  std::array<SDL_Texture*, 2> shown_{};       // as textures of the renderer.
  std::array<Kept, 4> kept_{};
  // downscaled()'s pictures, for the top and the bottom screen, and their sizes.
  std::array<SDL_GPUTexture*, 2> smaller_{};
  std::array<SDL_Texture*, 2> smaller_shown_{};
  std::array<std::array<int, 2>, 2> smaller_size_{};

  // The atlas: textures by key, placed on shelves from the top down.
  std::unordered_map<std::uint64_t, Place> placed_;
  int shelf_y_ = 0, shelf_x_ = 0, shelf_height_ = 0;
  bool atlas_full_noted_ = false;
  int log_compose_column_ = 0;  // The frame log's column for the next compose.
  std::uint64_t scene_drawn_ = 0;  // The serial of the scene in scene_target_; 0 for none.
  std::vector<std::uint32_t> pending_;  // Indices of the scene's textures to upload.
};

}  // namespace bd::app
