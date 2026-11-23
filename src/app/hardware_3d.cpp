#include "app/hardware_3d.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>

#include "app/hardware_shaders.h"
#include "debug/frame_log.h"
#include "nds/polygon_renderer.h"
#include "core/log.h"

namespace bd::app {

namespace {

using Scene = nds::HardwareScene;

constexpr int kAtlasSize = 2048;
constexpr int kScreenWidth = 256, kScreenHeight = 192;
constexpr std::uint32_t kScreenBytes = kScreenWidth * kScreenHeight * 4;
static_assert(sizeof(Scene::Vertex) == 44, "the vertex layout the shaders read");

// The scene shader's uniforms, as std140 lays them out.
struct SceneUniforms {
  float toon[32][4];
  float options[4];  // The alpha under which nothing is drawn; 1 for highlight; 1 for opaque; 1 for depth by w.
};
constexpr std::uint32_t kBankBytes = 256 * 256 * 4;
static_assert(sizeof(Scene::Raster::Corner) == 24 && sizeof(Scene::Raster::Polygon) == 32,
              "the layout the rasterizer reads");

// The rasterizer's uniforms (shaders/raster.comp, finish.comp), as std140
// lays them out.
struct RasterUniforms {
  std::uint32_t size[4];     // Width, height, scale, polygons.
  std::uint32_t control[4];  // The control register, the alpha test's reference, 1 for w, edge width.
  std::uint32_t clear[4];    // Colour, depth, number (+256 translucent, +512 fog).
  std::uint32_t toon[8][4];
};
struct FinishUniforms {
  std::uint32_t size[4];     // Width, height, what is done (finish.comp), outline width.
  std::uint32_t clear[4];    // Number, depth.
  std::uint32_t edge_colours[2][4];
  std::uint32_t fog[4];      // Colour, offset, step.
  std::uint32_t fog_table[8][4];
};
constexpr std::uint32_t kRasterGroup = 64;  // Pixels of a line in a workgroup.

using ComposeUniforms = Hardware3D::ComposeUniforms;

// The clear depth as the scene shader writes depths: 24 bits.
std::uint32_t clear_depth_bits(const Scene& scene) {
  return static_cast<std::uint32_t>(std::clamp(scene.clear_depth, 0.0f, 1.0f) * 16777215.0f + 0.5f);
}

// Which of a run's pixels are drawn, as the scene shader's u_options.z
// says: 0 all, as they are; 1 all, as opaque ones; 2 those of less than full
// alpha; 3 those of full alpha, as opaque ones.
int pixels_drawn(Scene::Kind kind) {
  switch (kind) {
    case Scene::Kind::Opaque: return 1;
    case Scene::Kind::Translucent:
    case Scene::Kind::Overlay: return 2;
    case Scene::Kind::Solid: return 3;
    default: return 0;
  }
}

// What a run writes in the stencil buffer, or compares with it (the
// pipelines say which bits).
Uint8 stencil_reference(const Scene::Run& run) {
  const auto number = static_cast<Uint8>(run.number & 63);
  switch (run.kind) {
    case Scene::Kind::Opaque:
    case Scene::Kind::Solid:
    case Scene::Kind::ShadowGuard: return number;
    case Scene::Kind::Translucent:
    case Scene::Kind::Overlay: return static_cast<Uint8>(0x40 | number);
    case Scene::Kind::ShadowClear: return 0;
    case Scene::Kind::ShadowMask: return 0x80;
    case Scene::Kind::Shadow: return static_cast<Uint8>(0xC0 | number);
  }
  return 0;
}

std::uint32_t aligned(std::uint32_t n) { return (n + 255u) & ~255u; }

SDL_GPUShader* make_shader(SDL_GPUDevice* device, const std::uint8_t* code, std::size_t size, SDL_GPUShaderStage stage,
                           std::uint32_t samplers, std::uint32_t uniforms, std::uint32_t storage_buffers = 0) {
  SDL_GPUShaderCreateInfo info{};
  info.code_size = size;
  info.code = code;
  info.entrypoint = "main";
  info.format = SDL_GPU_SHADERFORMAT_SPIRV;
  info.stage = stage;
  info.num_samplers = samplers;
  info.num_storage_buffers = storage_buffers;
  info.num_uniform_buffers = uniforms;
  return SDL_CreateGPUShader(device, &info);
}

SDL_GPUComputePipeline* make_compute(SDL_GPUDevice* device, const std::uint8_t* code, std::size_t size,
                                     std::uint32_t samplers, std::uint32_t read_buffers, std::uint32_t written_buffers) {
  SDL_GPUComputePipelineCreateInfo info{};
  info.code_size = size;
  info.code = code;
  info.entrypoint = "main";
  info.format = SDL_GPU_SHADERFORMAT_SPIRV;
  info.num_samplers = samplers;
  info.num_readonly_storage_buffers = read_buffers;
  info.num_readwrite_storage_buffers = written_buffers;
  info.num_uniform_buffers = 1;
  info.threadcount_x = kRasterGroup;
  info.threadcount_y = 1;
  info.threadcount_z = 1;
  return SDL_CreateGPUComputePipeline(device, &info);
}

SDL_GPUTexture* make_texture(SDL_GPUDevice* device, SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage, int width,
                             int height) {
  SDL_GPUTextureCreateInfo info{};
  info.type = SDL_GPU_TEXTURETYPE_2D;
  info.format = format;
  info.usage = usage;
  info.width = static_cast<Uint32>(width);
  info.height = static_cast<Uint32>(height);
  info.layer_count_or_depth = 1;
  info.num_levels = 1;
  info.sample_count = SDL_GPU_SAMPLECOUNT_1;
  return SDL_CreateGPUTexture(device, &info);
}

}  // namespace

Hardware3D::~Hardware3D() {
  if (device_ == nullptr) return;
  release_targets();
  for (SDL_GPUGraphicsPipeline* p : scene_pipelines_)
    if (p != nullptr) SDL_ReleaseGPUGraphicsPipeline(device_, p);
  if (compose_pipeline_ != nullptr) SDL_ReleaseGPUGraphicsPipeline(device_, compose_pipeline_);
  for (SDL_GPUComputePipeline* p : {raster_pipeline_, finish_pipeline_})
    if (p != nullptr) SDL_ReleaseGPUComputePipeline(device_, p);
  if (nearest_ != nullptr) SDL_ReleaseGPUSampler(device_, nearest_);
  if (atlas_ != nullptr) SDL_ReleaseGPUTexture(device_, atlas_);
  if (screen_ != nullptr) SDL_ReleaseGPUTexture(device_, screen_);
  if (bank_ != nullptr) SDL_ReleaseGPUTexture(device_, bank_);
  if (transfer_ != nullptr) SDL_ReleaseGPUTransferBuffer(device_, transfer_);
  for (SDL_GPUBuffer* b : {vertices_, corners_, polygons_})
    if (b != nullptr) SDL_ReleaseGPUBuffer(device_, b);
}

bool Hardware3D::open(SDL_Renderer* renderer, std::string& error) {
  SDL_GPUDevice* device = SDL_GetGPURendererDevice(renderer);
  if (device == nullptr) {
    error = "the window is not drawn through SDL's GPU API";
    return false;
  }
  if ((SDL_GetGPUShaderFormats(device) & SDL_GPU_SHADERFORMAT_SPIRV) == 0) {
    error = std::string("the graphics device (") + SDL_GetGPUDeviceDriver(device) + ") does not take SPIR-V shaders";
    return false;
  }
  renderer_ = renderer;
  device_ = device;
  const auto fail = [&](const char* what) {
    error = std::string("cannot make the ") + what + ": " + SDL_GetError();
    return false;
  };

  SDL_GPUShader* scene_vertex = make_shader(device, shaders::kSceneVert, sizeof shaders::kSceneVert,
                                            SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
  SDL_GPUShader* scene_fragment = make_shader(device, shaders::kSceneFrag, sizeof shaders::kSceneFrag,
                                              SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
  SDL_GPUShader* compose_vertex = make_shader(device, shaders::kComposeVert, sizeof shaders::kComposeVert,
                                              SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
  SDL_GPUShader* compose_fragment = make_shader(device, shaders::kComposeFrag, sizeof shaders::kComposeFrag,
                                                SDL_GPU_SHADERSTAGE_FRAGMENT, 5, 1, 1);
  const bool shaders_made = scene_vertex && scene_fragment && compose_vertex && compose_fragment;

  // The depth buffer: the most exact format the device has, with a stencil
  // buffer if it can.
  for (SDL_GPUTextureFormat f : {SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT, SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT,
                                 SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTUREFORMAT_D24_UNORM,
                                 SDL_GPU_TEXTUREFORMAT_D16_UNORM}) {
    if (SDL_GPUTextureSupportsFormat(device, f, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
      depth_format_ = f;
      stencil_ = f == SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT || f == SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
      break;
    }
  }

  if (shaders_made) {
    SDL_GPUVertexBufferDescription buffer{};
    buffer.slot = 0;
    buffer.pitch = sizeof(Scene::Vertex);
    buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    const std::array<SDL_GPUVertexAttribute, 4> attributes{{
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(Scene::Vertex, position)},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Scene::Vertex, texel)},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(Scene::Vertex, colour)},
        {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT4, offsetof(Scene::Vertex, info)},
    }};
    for (int index = 0; index < static_cast<int>(std::size(scene_pipelines_)); ++index) {
      using Kind = Scene::Kind;
      // The pixels of full alpha of a translucent polygon are drawn as opaque ones.
      const auto kind = static_cast<Kind>(index / 2) == Kind::Solid ? Kind::Opaque : static_cast<Kind>(index / 2);
      const bool coloured = kind == Kind::Opaque || kind == Kind::Translucent || kind == Kind::Overlay || kind == Kind::Shadow;
      // The colour, and the depths and fog marks (the depth where the
      // polygon keeps its depth, the mark times the polygon's: kept where
      // every polygon drawn on the pixel takes fog), and the numbers of
      // opaque polygons (for outlines above the DS's resolution).
      std::array<SDL_GPUColorTargetDescription, 3> targets{};
      SDL_GPUColorTargetDescription& target = targets[0];
      for (SDL_GPUColorTargetDescription& t : targets) t.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
      SDL_GPUColorTargetBlendState& fog = targets[1].blend_state;
      if (kind != Kind::Opaque) {
        targets[2].blend_state.enable_color_write_mask = true;
        targets[2].blend_state.color_write_mask = 0;
      }
      if (!coloured) {
        fog.enable_color_write_mask = true;
        fog.color_write_mask = 0;
      } else if (kind != Kind::Opaque) {
        fog.enable_blend = true;
        fog.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        fog.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        fog.color_blend_op = SDL_GPU_BLENDOP_ADD;
        fog.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        fog.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        fog.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        if (kind != Kind::Translucent) {
          fog.enable_color_write_mask = true;
          fog.color_write_mask = SDL_GPU_COLORCOMPONENT_A;
        }
      }
      if (!coloured) {
        target.blend_state.enable_color_write_mask = true;
        target.blend_state.color_write_mask = 0;
      } else if (kind != Kind::Opaque) {
        // Colours are kept times their alpha: "over".
        target.blend_state.enable_blend = true;
        target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
      }
      SDL_GPUGraphicsPipelineCreateInfo info{};
      info.vertex_shader = scene_vertex;
      info.fragment_shader = scene_fragment;
      info.vertex_input_state.vertex_buffer_descriptions = &buffer;
      info.vertex_input_state.num_vertex_buffers = 1;
      info.vertex_input_state.vertex_attributes = attributes.data();
      info.vertex_input_state.num_vertex_attributes = static_cast<Uint32>(attributes.size());
      info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
      info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
      info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;  // The geometry engine has culled.
      info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
      info.depth_stencil_state.enable_depth_test = kind != Kind::ShadowClear && kind != Kind::ShadowGuard;
      info.depth_stencil_state.enable_depth_write = kind == Kind::Opaque || kind == Kind::Translucent;
      info.depth_stencil_state.compare_op = index % 2 != 0 ? SDL_GPU_COMPAREOP_LESS_OR_EQUAL : SDL_GPU_COMPAREOP_LESS;
      if (stencil_) {
        // A byte for each pixel (stencil_reference() gives what is written):
        // the low six bits, the number of the last polygon drawn on it; 64,
        // that polygon is translucent; 128, marked by a shadow mask.
        SDL_GPUStencilOpState op{};
        op.fail_op = SDL_GPU_STENCILOP_KEEP;
        op.depth_fail_op = SDL_GPU_STENCILOP_KEEP;
        op.pass_op = SDL_GPU_STENCILOP_REPLACE;
        op.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
        Uint8 compare = 0, write = 0x7F;
        switch (kind) {
          case Kind::Opaque:
          case Kind::Solid: break;
          case Kind::Translucent:
          case Kind::Overlay:
            // Not over translucent pixels of the same polygon number.
            op.compare_op = SDL_GPU_COMPAREOP_NOT_EQUAL;
            compare = 0x7F;
            break;
          case Kind::ShadowClear: write = 0x80; break;
          case Kind::ShadowMask:
            op.pass_op = SDL_GPU_STENCILOP_KEEP;
            op.depth_fail_op = SDL_GPU_STENCILOP_REPLACE;
            write = 0x80;
            break;
          case Kind::ShadowGuard:
            op.compare_op = SDL_GPU_COMPAREOP_EQUAL;
            compare = 0x7F;
            write = 0x80;
            break;
          case Kind::Shadow:
            op.compare_op = SDL_GPU_COMPAREOP_EQUAL;
            compare = 0x80;
            break;
        }
        info.depth_stencil_state.front_stencil_state = op;
        info.depth_stencil_state.back_stencil_state = op;
        info.depth_stencil_state.compare_mask = compare;
        info.depth_stencil_state.write_mask = write;
        info.depth_stencil_state.enable_stencil_test = true;
      }
      info.target_info.color_target_descriptions = targets.data();
      info.target_info.num_color_targets = static_cast<Uint32>(targets.size());
      info.target_info.depth_stencil_format = depth_format_;
      info.target_info.has_depth_stencil_target = true;
      scene_pipelines_[index] = SDL_CreateGPUGraphicsPipeline(device, &info);
    }
    SDL_GPUColorTargetDescription target{};
    target.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = compose_vertex;
    info.fragment_shader = compose_fragment;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.target_info.color_target_descriptions = &target;
    info.target_info.num_color_targets = 1;
    compose_pipeline_ = SDL_CreateGPUGraphicsPipeline(device, &info);
    raster_pipeline_ = make_compute(device, shaders::kRasterComp, sizeof shaders::kRasterComp, 1, 2, 1);
    finish_pipeline_ = make_compute(device, shaders::kFinishComp, sizeof shaders::kFinishComp, 0, 1, 1);
  }
  for (SDL_GPUShader* s : {scene_vertex, scene_fragment, compose_vertex, compose_fragment})
    if (s != nullptr) SDL_ReleaseGPUShader(device, s);

  bool made = shaders_made;
  if (!made) {
    fail("3D shaders");
  } else if (depth_format_ == SDL_GPU_TEXTUREFORMAT_INVALID) {
    error = "the graphics device has no depth buffer format";
    made = false;
  } else if (std::count(std::begin(scene_pipelines_), std::end(scene_pipelines_), nullptr) != 0 || !compose_pipeline_ ||
             !raster_pipeline_ || !finish_pipeline_) {
    made = fail("3D pipelines");
  }
  if (made) {
    SDL_GPUSamplerCreateInfo sampler{};
    sampler.min_filter = SDL_GPU_FILTER_NEAREST;
    sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    nearest_ = SDL_CreateGPUSampler(device, &sampler);
    atlas_ = make_texture(device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER, kAtlasSize, kAtlasSize);
    screen_ = make_texture(device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER, kScreenWidth,
                           kScreenHeight);
    bank_ = make_texture(device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER, 256, 256);
    if (nearest_ == nullptr || atlas_ == nullptr || screen_ == nullptr || bank_ == nullptr) made = fail("3D textures");
  }
  if (!made) return false;  // What was made is let go by the destructor.
  log::writef(log::Level::Info, "3d", "the graphics hardware draws 3D scenes (%s, depth buffer format %d, %s)",
              SDL_GetGPUDeviceDriver(device), static_cast<int>(depth_format_),
              stencil_ ? "with polygon numbers and shadows" : "without polygon numbers or shadows");
  return true;
}

void Hardware3D::release_targets() {
  for (SDL_Texture*& t : shown_) {
    if (t != nullptr) SDL_DestroyTexture(t);
    t = nullptr;
  }
  for (SDL_GPUTexture*& t : outputs_) {
    if (t != nullptr) SDL_ReleaseGPUTexture(device_, t);
    t = nullptr;
  }
  for (Kept& k : kept_) {
    if (k.texture != nullptr) SDL_ReleaseGPUTexture(device_, k.texture);
    k = Kept{};
  }
  for (SDL_GPUTexture* t : {scene_target_, fog_marks_, numbers_, depth_})
    if (t != nullptr) SDL_ReleaseGPUTexture(device_, t);
  scene_target_ = fog_marks_ = numbers_ = depth_ = nullptr;
  for (SDL_GPUBuffer* b : {layers_, finished_})
    if (b != nullptr) SDL_ReleaseGPUBuffer(device_, b);
  layers_ = finished_ = nullptr;
  scale_ = 0;
  scene_drawn_ = 0;
  rasterized_ = false;
}

bool Hardware3D::make_targets(int scale) {
  release_targets();
  const int width = kScreenWidth * scale, height = kScreenHeight * scale;
  const SDL_GPUTextureUsageFlags colour = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
  bool made = true;
  const auto make = [&](SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage, int lines = kScreenHeight) {
    SDL_GPUTexture* t = make_texture(device_, format, usage, width, lines * scale);
    made = made && t != nullptr;
    return t;
  };
  scene_target_ = make(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, colour);
  fog_marks_ = make(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, colour);
  numbers_ = make(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, colour);
  depth_ = make(depth_format_, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
  const auto make_buffer = [&](SDL_GPUBufferUsageFlags usage, std::uint32_t bytes_a_pixel) {
    SDL_GPUBufferCreateInfo info{};
    info.usage = usage;
    info.size = static_cast<Uint32>(width) * static_cast<Uint32>(height) * bytes_a_pixel;
    SDL_GPUBuffer* b = SDL_CreateGPUBuffer(device_, &info);
    made = made && b != nullptr;
    return b;
  };
  layers_ = make_buffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, 16);
  finished_ = make_buffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, 4);
  // A kept capture is its bank of video memory, 256 by 256.
  for (Kept& k : kept_) k.texture = make(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, colour, 256);
  for (std::size_t i = 0; i < outputs_.size() && made; ++i) {
    outputs_[i] = make(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, colour);
    if (!made) break;
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER, outputs_[i]);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_RGBA32);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STATIC);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, width);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, height);
    shown_[i] = SDL_CreateTextureWithProperties(renderer_, props);
    SDL_DestroyProperties(props);
    if (shown_[i] == nullptr) made = false;
    else SDL_SetTextureBlendMode(shown_[i], SDL_BLENDMODE_NONE);
  }
  if (!made) {
    log::warning("3d", std::string("cannot make the 3D targets at ") + std::to_string(scale) + " times: " + SDL_GetError());
    release_targets();
    return false;
  }
  scale_ = scale;
  log::writef(log::Level::Info, "3d", "3D scenes drawn at %d times the DS's resolution (%d by %d)", scale, width, height);
  return true;
}

bool Hardware3D::allocate(int width, int height, Place& place) {
  if (width > kAtlasSize || height > kAtlasSize) return false;
  if (shelf_x_ + width > kAtlasSize) {
    shelf_y_ += shelf_height_;
    shelf_x_ = shelf_height_ = 0;
  }
  if (shelf_y_ + height > kAtlasSize) return false;
  place = {static_cast<std::uint16_t>(shelf_x_), static_cast<std::uint16_t>(shelf_y_)};
  shelf_x_ += width;
  shelf_height_ = std::max(shelf_height_, height);
  return true;
}

bool Hardware3D::ensure_transfer(std::uint32_t bytes) {
  if (transfer_ != nullptr && transfer_size_ >= bytes) return true;
  if (transfer_ != nullptr) SDL_ReleaseGPUTransferBuffer(device_, transfer_);
  SDL_GPUTransferBufferCreateInfo info{};
  info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
  info.size = std::max(bytes, transfer_size_ * 2);
  transfer_ = SDL_CreateGPUTransferBuffer(device_, &info);
  transfer_size_ = transfer_ != nullptr ? info.size : 0;
  return transfer_ != nullptr;
}

bool Hardware3D::ensure_vertex_buffer(std::uint32_t bytes) {
  if (vertices_ != nullptr && vertices_size_ >= bytes) return true;
  if (vertices_ != nullptr) SDL_ReleaseGPUBuffer(device_, vertices_);
  SDL_GPUBufferCreateInfo info{};
  info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
  info.size = std::max({bytes, vertices_size_ * 2, std::uint32_t{1} << 16});
  vertices_ = SDL_CreateGPUBuffer(device_, &info);
  vertices_size_ = vertices_ != nullptr ? info.size : 0;
  return vertices_ != nullptr;
}

bool Hardware3D::ensure_storage(SDL_GPUBuffer*& buffer, std::uint32_t& size, std::uint32_t bytes) {
  if (buffer != nullptr && size >= bytes) return true;
  if (buffer != nullptr) SDL_ReleaseGPUBuffer(device_, buffer);
  SDL_GPUBufferCreateInfo info{};
  info.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
  info.size = std::max({bytes, size * 2, std::uint32_t{1} << 12});
  buffer = SDL_CreateGPUBuffer(device_, &info);
  size = buffer != nullptr ? info.size : 0;
  return buffer != nullptr;
}

bool Hardware3D::ready(int scale) {
  if (device_ == nullptr) return false;
  scale = std::clamp(scale, 1, kLargestScale);
  return scale == scale_ || make_targets(scale);
}

bool Hardware3D::capture(const nds::HardwareScene& scene, const nds::CaptureRecipe& recipe,
                         const std::vector<std::uint32_t>& source, std::uint64_t number, int scale) {
  if (!ready(scale)) return false;
  // The second source: a kept capture, or the bank as it is.
  SDL_GPUTexture* second = nullptr;
  const Kept* read = nullptr;
  for (const Kept& k : kept_)
    if (recipe.second_capture != 0 && k.number == recipe.second_capture) read = &k;
  if (read != nullptr) second = read->texture;
  else if (recipe.second_pixels.size() == 0x10000) second = bank_;
  // In place of the oldest, but not the one it reads.
  Kept* oldest = nullptr;
  for (Kept& k : kept_)
    if (&k != read && (oldest == nullptr || k.number < oldest->number)) oldest = &k;
  oldest->number = 0;
  ComposeUniforms uniforms{};
  uniforms.options[0] = 1;
  uniforms.capture[0] = std::min(recipe.source, 2u) | (recipe.only_3d ? 4u : 0u);
  uniforms.capture[1] = recipe.weight_first | recipe.weight_second << 8;
  uniforms.capture[2] = recipe.width | recipe.height << 16;
  uniforms.capture[3] = (recipe.write_start & 0xFFFF) | (recipe.read_start & 0xFFFF) << 16;
  if (recipe.source != 0 && second == nullptr) uniforms.capture[0] = recipe.only_3d ? 4u : 0u;
  log_compose_column_ = frame_log::kCaptureGpu;
  ++frame_log::state().counts.gpu_captures;
  const std::vector<std::uint32_t>* bank = second == bank_ ? &recipe.second_pixels : nullptr;
  if (!run(&scene, source, nullptr, second, bank, oldest->texture, uniforms)) return false;
  oldest->number = number;
  return true;
}

SDL_Texture* Hardware3D::draw(int which, const std::vector<std::uint32_t>& screen, const nds::HardwareScene* scene,
                              std::uint64_t capture, std::uint16_t brightness, int scale, bool& failed) {
  failed = true;
  if (!ready(scale)) return nullptr;
  SDL_GPUTexture* source = nullptr;
  if (scene == nullptr) {
    for (const Kept& k : kept_)
      if (k.number == capture && capture != 0) source = k.texture;
    if (source == nullptr) {
      failed = false;
      return nullptr;
    }
  }
  ComposeUniforms uniforms{};
  const unsigned direction = brightness >> 14;
  uniforms.brightness[0] = direction == 1 || direction == 2 ? static_cast<float>(direction) : 0.0f;
  uniforms.brightness[1] = static_cast<float>(std::min(brightness & 0x1F, 16)) / 16.0f;
  uniforms.options[1] = source != nullptr ? 256.0f : static_cast<float>(kScreenHeight);
  log_compose_column_ = (which & 1) == 0 ? frame_log::kComposeTop : frame_log::kComposeBottom;
  if (!run(scene, screen, source, nullptr, nullptr, outputs_[static_cast<std::size_t>(which & 1)], uniforms))
    return nullptr;
  failed = false;
  return shown_[static_cast<std::size_t>(which & 1)];
}

bool Hardware3D::read_raster(std::vector<std::uint32_t>& pixels) {
  if (scale_ == 0 || !rasterized_) return false;
  pixels.resize(static_cast<std::size_t>(kScreenWidth) * scale_ * kScreenHeight * scale_);
  return download(nullptr, 0, 0, finished_, static_cast<std::uint32_t>(pixels.size() * 4), pixels.data());
}

bool Hardware3D::read_screen(int which, std::vector<std::uint32_t>& pixels) {
  if (scale_ == 0) return false;
  const int width = kScreenWidth * scale_, height = kScreenHeight * scale_;
  pixels.resize(static_cast<std::size_t>(width) * height);
  return download(outputs_[static_cast<std::size_t>(which & 1)], width, height, nullptr,
                  static_cast<std::uint32_t>(pixels.size() * 4), pixels.data());
}

bool Hardware3D::download(SDL_GPUTexture* texture, int width, int height, SDL_GPUBuffer* buffer, std::uint32_t bytes,
                          void* out) {
  SDL_GPUTransferBufferCreateInfo info{};
  info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
  info.size = bytes;
  SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &info);
  if (transfer == nullptr) return false;
  bool done = false;
  if (SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_)) {
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    if (texture != nullptr) {
      SDL_GPUTextureRegion from{};
      from.texture = texture;
      from.w = static_cast<Uint32>(width);
      from.h = static_cast<Uint32>(height);
      from.d = 1;
      SDL_GPUTextureTransferInfo to{};
      to.transfer_buffer = transfer;
      SDL_DownloadFromGPUTexture(copy, &from, &to);
    } else {
      const SDL_GPUBufferRegion from{buffer, 0, bytes};
      const SDL_GPUTransferBufferLocation to{transfer, 0};
      SDL_DownloadFromGPUBuffer(copy, &from, &to);
    }
    SDL_EndGPUCopyPass(copy);
    if (SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands)) {
      done = SDL_WaitForGPUFences(device_, true, &fence, 1);
      SDL_ReleaseGPUFence(device_, fence);
    }
  }
  if (done) {
    if (const void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false)) {
      std::memcpy(out, mapped, bytes);
      SDL_UnmapGPUTransferBuffer(device_, transfer);
    } else {
      done = false;
    }
  }
  SDL_ReleaseGPUTransferBuffer(device_, transfer);
  return done;
}

bool Hardware3D::run(const nds::HardwareScene* given, const std::vector<std::uint32_t>& screen, SDL_GPUTexture* source,
                     SDL_GPUTexture* second, const std::vector<std::uint32_t>* bank, SDL_GPUTexture* target,
                     const ComposeUniforms& given_uniforms) {
  if (screen.size() != std::size_t{kScreenWidth} * kScreenHeight) return false;
  // The frame log's times. In sync mode the GPU is idle before the passes,
  // and each pass is handed over and waited for on its own.
  frame_log::Scope frame_log_scope(frame_log::kUpload);
  if (frame_log::state().sync) {
    frame_log::Scope wait(frame_log::kPresentGpu);
    SDL_WaitForGPUIdle(device_);
  }
  static const nds::HardwareScene kNoScene;
  const nds::HardwareScene& scene = given != nullptr ? *given : kNoScene;
  const bool new_scene = given != nullptr && scene.serial != scene_drawn_;

  // The textures the scene needs that the atlas does not have. When it is
  // full it starts again, empty, with this scene's.
  pending_.clear();
  std::vector<Place> places(scene.textures.size());
  std::vector<bool> placed(scene.textures.size(), false);
  if (new_scene) {
    for (int attempt = 0; attempt < 2; ++attempt) {
      pending_.clear();
      bool full = false;
      for (std::size_t i = 0; i < scene.textures.size(); ++i) {
        const Scene::Texture& t = scene.textures[i];
        if (const auto it = placed_.find(t.key); it != placed_.end()) {
          places[i] = it->second;
          placed[i] = true;
        } else if (allocate(t.width, t.height, places[i])) {
          placed_.emplace(t.key, places[i]);
          placed[i] = true;
          pending_.push_back(static_cast<std::uint32_t>(i));
        } else {
          full = true;
          placed[i] = false;
        }
      }
      if (!full || attempt == 1) {
        if (full && !atlas_full_noted_) {
          atlas_full_noted_ = true;
          log::warning("3d", "a scene's textures do not all fit in the texture atlas; some are left out");
        }
        break;
      }
      placed_.clear();
      shelf_x_ = shelf_y_ = shelf_height_ = 0;
      std::fill(placed.begin(), placed.end(), false);
    }
  }

  // What goes up: the screen, the vertices, the rasterizer's polygons and
  // the new textures.
  const std::uint32_t vertex_bytes = new_scene ? static_cast<std::uint32_t>(scene.vertices.size() * sizeof(Scene::Vertex)) : 0;
  const Scene::Raster& raster = scene.raster;
  // At the DS's resolution the rasterizer draws all of the scene, as the
  // processor does. At larger sizes it is not used: it would cost far more
  // than the triangles, and outlines are drawn from the triangles' pixels
  // (compose.frag).
  const bool rasterize = new_scene && scale_ == 1;
  const std::uint32_t corner_bytes =
      rasterize ? static_cast<std::uint32_t>(raster.corners.size() * sizeof(Scene::Raster::Corner)) : 0;
  const std::uint32_t polygon_bytes =
      rasterize ? static_cast<std::uint32_t>(raster.polygons.size() * sizeof(Scene::Raster::Polygon)) : 0;
  const std::uint32_t bank_bytes = bank != nullptr ? kBankBytes : 0;
  std::uint32_t bytes = kScreenBytes + aligned(vertex_bytes) + bank_bytes + aligned(corner_bytes) + aligned(polygon_bytes);
  for (std::uint32_t i : pending_)
    bytes += aligned(static_cast<std::uint32_t>(scene.textures[i].width * scene.textures[i].height * 4));
  if (!ensure_transfer(bytes) || (vertex_bytes != 0 && !ensure_vertex_buffer(vertex_bytes)) ||
      (rasterize && (!ensure_storage(corners_, corners_size_, corner_bytes) ||
                     !ensure_storage(polygons_, polygons_size_, polygon_bytes)))) {
    log::warning("3d", std::string("cannot make the buffers for a 3D scene: ") + SDL_GetError());
    return false;
  }
  {
    frame_log::Counts& c = frame_log::state().counts;
    c.screen_bytes += kScreenBytes;
    c.vertex_bytes += vertex_bytes;
    c.bank_bytes += bank_bytes;
    c.raster_bytes += std::uint64_t{corner_bytes} + polygon_bytes;
    c.textures_uploaded += pending_.size();
    for (std::uint32_t i : pending_) c.texture_bytes += scene.textures[i].pixels->size() * 4;
  }
  auto* mapped = static_cast<std::uint8_t*>(SDL_MapGPUTransferBuffer(device_, transfer_, true));
  if (mapped == nullptr) return false;
  // The screen: colours, with the marks (bits 24 to 31) as the alpha.
  for (std::size_t i = 0; i < screen.size(); ++i) {
    const std::uint32_t p = screen[i];
    const auto eight = [](std::uint32_t six) { return static_cast<std::uint8_t>((six & 0x3F) << 2 | (six & 0x3F) >> 4); };
    mapped[4 * i + 0] = eight(p);
    mapped[4 * i + 1] = eight(p >> 6);
    mapped[4 * i + 2] = eight(p >> 12);
    mapped[4 * i + 3] = static_cast<std::uint8_t>(p >> 24);
  }
  std::uint32_t at = kScreenBytes;
  const std::uint32_t bank_at = at;
  if (bank != nullptr) {
    for (std::size_t i = 0; i < 0x10000; ++i) {
      const std::uint32_t p = (*bank)[i];
      const auto eight = [](std::uint32_t six) { return static_cast<std::uint8_t>((six & 0x3F) << 2 | (six & 0x3F) >> 4); };
      mapped[at + 4 * i + 0] = eight(p);
      mapped[at + 4 * i + 1] = eight(p >> 6);
      mapped[at + 4 * i + 2] = eight(p >> 12);
      mapped[at + 4 * i + 3] = 255;
    }
    at += bank_bytes;
  }
  const std::uint32_t vertices_at = at;
  if (vertex_bytes != 0) {
    auto* out = reinterpret_cast<Scene::Vertex*>(mapped + at);
    for (std::size_t i = 0; i < scene.vertices.size(); ++i) {
      Scene::Vertex v = scene.vertices[i];
      if ((v.info[2] & Scene::kTextured) != 0) {
        const std::uint32_t t = v.info[0];
        if (t < places.size() && placed[t]) v.info[0] = places[t].x | std::uint32_t{places[t].y} << 16;
        else v.info[2] &= ~Scene::kTextured;
      }
      std::memcpy(&out[i], &v, sizeof v);
    }
    at += aligned(vertex_bytes);
  }
  const std::uint32_t corners_at = at;
  if (corner_bytes != 0) {
    std::memcpy(mapped + at, raster.corners.data(), corner_bytes);
    at += aligned(corner_bytes);
  }
  const std::uint32_t polygons_at = at;
  if (polygon_bytes != 0) {
    auto* out = reinterpret_cast<Scene::Raster::Polygon*>(mapped + at);
    for (std::size_t i = 0; i < raster.polygons.size(); ++i) {
      Scene::Raster::Polygon p = raster.polygons[i];
      if ((p.flags & Scene::Raster::kTextured) != 0) {
        const std::uint32_t t = p.texture_index;
        if (t < places.size() && placed[t]) p.texture_index = places[t].x | std::uint32_t{places[t].y} << 16;
        else p.flags &= ~Scene::Raster::kTextured;
      }
      std::memcpy(&out[i], &p, sizeof p);
    }
    at += aligned(polygon_bytes);
  }
  std::vector<std::uint32_t> texture_at;
  for (std::uint32_t i : pending_) {
    const Scene::Texture& t = scene.textures[i];
    texture_at.push_back(at);
    std::memcpy(mapped + at, t.pixels->data(), t.pixels->size() * 4);
    at += aligned(static_cast<std::uint32_t>(t.pixels->size() * 4));
  }
  SDL_UnmapGPUTransferBuffer(device_, transfer_);

  SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_);
  if (commands == nullptr) return false;
  // For the frame log: the pass recorded so far is done; the next one
  // begins. In sync mode the recorded one is handed over and waited for.
  const auto frame_log_pass = [&](int next) -> bool {
    if (frame_log::state().sync) {
      SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
      if (fence == nullptr) return false;
      SDL_WaitForGPUFences(device_, true, &fence, 1);
      SDL_ReleaseGPUFence(device_, fence);
      commands = SDL_AcquireGPUCommandBuffer(device_);
      if (commands == nullptr) return false;
    }
    if (frame_log::enabled()) frame_log::switch_to(next);
    return true;
  };
  SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
  SDL_GPUTextureTransferInfo from{};
  from.transfer_buffer = transfer_;
  SDL_GPUTextureRegion to{};
  to.texture = screen_;
  to.w = kScreenWidth;
  to.h = kScreenHeight;
  to.d = 1;
  SDL_UploadToGPUTexture(copy, &from, &to, true);
  if (bank != nullptr) {
    from.offset = bank_at;
    to.texture = bank_;
    to.w = to.h = 256;
    SDL_UploadToGPUTexture(copy, &from, &to, true);
  }
  if (vertex_bytes != 0) {
    const SDL_GPUTransferBufferLocation upload{transfer_, vertices_at};
    const SDL_GPUBufferRegion region{vertices_, 0, vertex_bytes};
    SDL_UploadToGPUBuffer(copy, &upload, &region, true);
  }
  if (corner_bytes != 0) {
    const SDL_GPUTransferBufferLocation upload{transfer_, corners_at};
    const SDL_GPUBufferRegion region{corners_, 0, corner_bytes};
    SDL_UploadToGPUBuffer(copy, &upload, &region, true);
  }
  if (polygon_bytes != 0) {
    const SDL_GPUTransferBufferLocation upload{transfer_, polygons_at};
    const SDL_GPUBufferRegion region{polygons_, 0, polygon_bytes};
    SDL_UploadToGPUBuffer(copy, &upload, &region, true);
  }
  for (std::size_t n = 0; n < pending_.size(); ++n) {
    const Scene::Texture& t = scene.textures[pending_[n]];
    from.offset = texture_at[n];
    to.texture = atlas_;
    to.x = places[pending_[n]].x;
    to.y = places[pending_[n]].y;
    to.w = static_cast<Uint32>(t.width);
    to.h = static_cast<Uint32>(t.height);
    SDL_UploadToGPUTexture(copy, &from, &to, false);
  }
  SDL_EndGPUCopyPass(copy);
  if (!frame_log_pass(new_scene ? frame_log::kScenePass : log_compose_column_)) return false;

  if (new_scene) {
    // 1. The scene, over the clear colour (times its alpha) at the clear depth.
    std::array<SDL_GPUColorTargetInfo, 3> colours{};
    SDL_GPUColorTargetInfo& colour = colours[0];
    colour.texture = scene_target_;
    const float a = scene.clear_colour[3];
    colour.clear_color = {scene.clear_colour[0] * a, scene.clear_colour[1] * a, scene.clear_colour[2] * a, a};
    colour.load_op = SDL_GPU_LOADOP_CLEAR;
    colour.store_op = SDL_GPU_STOREOP_STORE;
    colour.cycle = true;
    // The clear plane's depth, and whether it takes fog.
    const std::uint32_t clear_depth = clear_depth_bits(scene);
    colours[1] = colour;
    colours[1].texture = fog_marks_;
    colours[1].clear_color = {static_cast<float>(clear_depth >> 16) / 255.0f,
                              static_cast<float>((clear_depth >> 8) & 255) / 255.0f,
                              static_cast<float>(clear_depth & 255) / 255.0f, scene.clear_fog ? 1.0f : 0.0f};
    // The clear plane's polygon number; no opaque polygon there yet.
    colours[2] = colour;
    colours[2].texture = numbers_;
    colours[2].clear_color = {static_cast<float>(scene.clear_number & 63) / 63.0f, 0.0f, 0.0f, 0.0f};
    SDL_GPUDepthStencilTargetInfo depth{};
    depth.texture = depth_;
    depth.clear_depth = scene.clear_depth;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.stencil_load_op = SDL_GPU_LOADOP_CLEAR;
    depth.clear_stencil = 0;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.cycle = true;
    SDL_GPURenderPass* pass =
        SDL_BeginGPURenderPass(commands, colours.data(), static_cast<Uint32>(colours.size()), &depth);
    if (!scene.runs.empty()) {
      const SDL_GPUBufferBinding binding{vertices_, 0};
      SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
      const SDL_GPUTextureSamplerBinding atlas{atlas_, nearest_};
      SDL_BindGPUFragmentSamplers(pass, 0, &atlas, 1);
      SceneUniforms uniforms{};
      for (std::size_t i = 0; i < scene.toon.size(); ++i) std::memcpy(uniforms.toon[i], scene.toon[i].data(), sizeof uniforms.toon[i]);
      uniforms.options[0] = scene.alpha_threshold;
      uniforms.options[1] = scene.highlight ? 1.0f : 0.0f;
      uniforms.options[3] = scene.depth_is_w ? 1.0f : 0.0f;
      for (const Scene::Run& run : scene.runs) {
        if (!stencil_ && run.kind >= Scene::Kind::ShadowClear) continue;
        uniforms.options[2] = static_cast<float>(pixels_drawn(run.kind));
        SDL_BindGPUGraphicsPipeline(pass, scene_pipelines_[static_cast<int>(run.kind) * 2 + (run.depth_equal ? 1 : 0)]);
        if (stencil_) SDL_SetGPUStencilReference(pass, stencil_reference(run));
        SDL_PushGPUFragmentUniformData(commands, 0, &uniforms, sizeof uniforms);
        SDL_DrawGPUPrimitives(pass, run.count, 1, run.first, 0);
      }
    }
    SDL_EndGPURenderPass(pass);
    scene_drawn_ = scene.serial;
    rasterized_ = rasterize;
    if (rasterize) {
      if (!frame_log_pass(frame_log::kRaster)) return false;
      frame_log::state().counts.raster_ran = true;
      rasterize_scene(commands, scene);
    }
    if (!frame_log_pass(log_compose_column_)) return false;
  }

  // 2. The screen with the scene (or the kept capture) put in.
  SDL_GPUColorTargetInfo colour{};
  colour.texture = target;
  colour.load_op = SDL_GPU_LOADOP_DONT_CARE;
  colour.store_op = SDL_GPU_STOREOP_STORE;
  colour.cycle = true;
  SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &colour, 1, nullptr);
  SDL_BindGPUGraphicsPipeline(pass, compose_pipeline_);
  const std::array<SDL_GPUTextureSamplerBinding, 5> sources{{{screen_, nearest_},
                                                             {source != nullptr ? source : scene_target_, nearest_},
                                                             {second != nullptr ? second : bank_, nearest_},
                                                             {fog_marks_, nearest_},
                                                             {numbers_, nearest_}}};
  SDL_BindGPUFragmentSamplers(pass, 0, sources.data(), static_cast<Uint32>(sources.size()));
  SDL_BindGPUFragmentStorageBuffers(pass, 0, &finished_, 1);
  // Outlines and anti-aliased edges where the scene is put in.
  ComposeUniforms uniforms = given_uniforms;
  if (given != nullptr && source == nullptr && rasterized_) {
    uniforms.edge[0] = static_cast<std::uint32_t>(kScreenWidth * scale_);
    uniforms.edge[1] = static_cast<std::uint32_t>(kScreenHeight * scale_);
    uniforms.edge[3] = 1;
  }
  // Above the DS's resolution, outlines where the scene is put in: solid,
  // in the edge colours, as wide as asked.
  if (given != nullptr && source == nullptr && scene.edge_marking && scale_ > 1) {
    using Marking = nds::PolygonRenderer::EdgeMarking;
    uniforms.outline[0] = static_cast<std::uint32_t>(
        nds::PolygonRenderer::edge_width(scene.edge_one_pixel ? Marking::OnePixel : Marking::OriginalThickness, scale_));
    uniforms.outline[1] = scene.clear_number & 63u;
    uniforms.outline[2] = clear_depth_bits(scene);
    uniforms.outline[3] = static_cast<std::uint32_t>(scale_);
    std::memcpy(uniforms.outline_colours, scene.edge_colours.data(), sizeof uniforms.outline_colours);
  }
  // Fog, where the scene is put in.
  if (given != nullptr && source == nullptr && scene.fog) {
    uniforms.fog[0] = 1 | (scene.fog_alpha_only ? 2u : 0u);
    uniforms.fog[1] = scene.fog_offset;
    uniforms.fog[2] = scene.fog_step;
    std::memcpy(uniforms.fog_colour, scene.fog_colour.data(), sizeof uniforms.fog_colour);
    std::memcpy(uniforms.clear_colour, scene.clear_colour.data(), sizeof uniforms.clear_colour);
    for (std::size_t i = 0; i < scene.fog_table.size(); ++i) uniforms.fog_table[i / 4][i % 4] = scene.fog_table[i];
  }
  SDL_PushGPUFragmentUniformData(commands, 0, &uniforms, sizeof uniforms);
  SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
  SDL_EndGPURenderPass(pass);
  if (frame_log::state().sync) {
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    if (fence == nullptr) {
      log::warning("3d", std::string("cannot hand a 3D scene to the graphics hardware: ") + SDL_GetError());
      return false;
    }
    SDL_WaitForGPUFences(device_, true, &fence, 1);
    SDL_ReleaseGPUFence(device_, fence);
    return true;
  }
  if (!SDL_SubmitGPUCommandBuffer(commands)) {
    log::warning("3d", std::string("cannot hand a 3D scene to the graphics hardware: ") + SDL_GetError());
    return false;
  }
  return true;
}

void Hardware3D::rasterize_scene(SDL_GPUCommandBuffer* commands, const nds::HardwareScene& scene) {
  const Scene::Raster& raster = scene.raster;
  const auto width = static_cast<std::uint32_t>(kScreenWidth * scale_), height = static_cast<std::uint32_t>(kScreenHeight * scale_);
  using Marking = nds::PolygonRenderer::EdgeMarking;
  const auto edge_width = static_cast<std::uint32_t>(
      scene.edge_marking
          ? nds::PolygonRenderer::edge_width(scene.edge_one_pixel ? Marking::OnePixel : Marking::OriginalThickness, scale_)
          : 1);

  // 1. The polygons, into two layers a pixel.
  RasterUniforms drawing{};
  drawing.size[0] = width;
  drawing.size[1] = height;
  drawing.size[2] = static_cast<std::uint32_t>(scale_);
  drawing.size[3] = static_cast<std::uint32_t>(raster.polygons.size());
  drawing.control[0] = raster.control;
  drawing.control[1] = raster.alpha_reference;
  drawing.control[2] = raster.depth_is_w ? 1 : 0;
  drawing.control[3] = edge_width;
  drawing.clear[0] = raster.clear_colour;
  drawing.clear[1] = raster.clear_depth;
  drawing.clear[2] = (scene.clear_number & 63u) | (raster.clear_translucent ? 256u : 0u) | (scene.clear_fog ? 512u : 0u);
  for (std::size_t i = 0; i < raster.toon.size(); ++i) drawing.toon[i / 4][i % 4] = raster.toon[i];
  SDL_GPUStorageBufferReadWriteBinding written{layers_, true, 0, 0, 0};
  SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, nullptr, 0, &written, 1);
  SDL_BindGPUComputePipeline(pass, raster_pipeline_);
  const SDL_GPUTextureSamplerBinding atlas{atlas_, nearest_};
  SDL_BindGPUComputeSamplers(pass, 0, &atlas, 1);
  SDL_GPUBuffer* const read[2] = {corners_, polygons_};
  SDL_BindGPUComputeStorageBuffers(pass, 0, read, 2);
  SDL_PushGPUComputeUniformData(commands, 0, &drawing, sizeof drawing);
  SDL_DispatchGPUCompute(pass, width / kRasterGroup, height, 1);
  SDL_EndGPUComputePass(pass);

  // 2. What is done when a line is done.
  FinishUniforms finishing{};
  finishing.size[0] = width;
  finishing.size[1] = height;
  finishing.size[2] = (scene.edge_marking ? 1u : 0u) | (scene.fog ? 2u : 0u) | (scene.fog_alpha_only ? 4u : 0u) |
                      (scene.anti_aliasing ? 8u : 0u) | (scale_ == 1 ? 16u : 0u);
  finishing.size[3] = edge_width;
  finishing.clear[0] = scene.clear_number & 63u;
  finishing.clear[1] = raster.clear_depth;
  for (std::size_t i = 0; i < raster.edge_colours.size(); ++i) finishing.edge_colours[i / 4][i % 4] = raster.edge_colours[i];
  finishing.fog[0] = raster.fog_colour;
  finishing.fog[1] = scene.fog_offset;
  finishing.fog[2] = scene.fog_step;
  for (std::size_t i = 0; i < scene.fog_table.size(); ++i) finishing.fog_table[i / 4][i % 4] = scene.fog_table[i];
  written = {finished_, true, 0, 0, 0};
  pass = SDL_BeginGPUComputePass(commands, nullptr, 0, &written, 1);
  SDL_BindGPUComputePipeline(pass, finish_pipeline_);
  SDL_BindGPUComputeStorageBuffers(pass, 0, &layers_, 1);
  SDL_PushGPUComputeUniformData(commands, 0, &finishing, sizeof finishing);
  SDL_DispatchGPUCompute(pass, width / kRasterGroup, height, 1);
  SDL_EndGPUComputePass(pass);
}

}  // namespace bd::app
