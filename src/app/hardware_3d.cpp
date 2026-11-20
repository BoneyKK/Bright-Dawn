#include "app/hardware_3d.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>

#include "app/hardware_shaders.h"
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
  float options[4];  // The alpha under which nothing is drawn; 1 for highlight; 1 for opaque.
};
struct ComposeUniforms {
  float brightness[4];  // 1 towards white, 2 towards black; how far (0 to 1).
};

std::uint32_t aligned(std::uint32_t n) { return (n + 255u) & ~255u; }

SDL_GPUShader* make_shader(SDL_GPUDevice* device, const std::uint8_t* code, std::size_t size, SDL_GPUShaderStage stage,
                           std::uint32_t samplers, std::uint32_t uniforms) {
  SDL_GPUShaderCreateInfo info{};
  info.code_size = size;
  info.code = code;
  info.entrypoint = "main";
  info.format = SDL_GPU_SHADERFORMAT_SPIRV;
  info.stage = stage;
  info.num_samplers = samplers;
  info.num_uniform_buffers = uniforms;
  return SDL_CreateGPUShader(device, &info);
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
  if (nearest_ != nullptr) SDL_ReleaseGPUSampler(device_, nearest_);
  if (atlas_ != nullptr) SDL_ReleaseGPUTexture(device_, atlas_);
  if (screen_ != nullptr) SDL_ReleaseGPUTexture(device_, screen_);
  if (transfer_ != nullptr) SDL_ReleaseGPUTransferBuffer(device_, transfer_);
  if (vertices_ != nullptr) SDL_ReleaseGPUBuffer(device_, vertices_);
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
                                                SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1);
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
    for (int index = 0; index < 6; ++index) {
      const int kind = index / 2;
      SDL_GPUColorTargetDescription target{};
      target.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
      if (kind != static_cast<int>(Scene::Kind::Opaque)) {
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
      info.depth_stencil_state.enable_depth_test = true;
      info.depth_stencil_state.enable_depth_write = kind != static_cast<int>(Scene::Kind::Overlay);
      info.depth_stencil_state.compare_op = index % 2 != 0 ? SDL_GPU_COMPAREOP_LESS_OR_EQUAL : SDL_GPU_COMPAREOP_LESS;
      if (stencil_ && kind != static_cast<int>(Scene::Kind::Opaque)) {
        // Not over translucent pixels of the same polygon number, which
        // each translucent pixel drawn leaves in the stencil buffer.
        SDL_GPUStencilOpState op{};
        op.fail_op = SDL_GPU_STENCILOP_KEEP;
        op.depth_fail_op = SDL_GPU_STENCILOP_KEEP;
        op.pass_op = SDL_GPU_STENCILOP_REPLACE;
        op.compare_op = SDL_GPU_COMPAREOP_NOT_EQUAL;
        info.depth_stencil_state.front_stencil_state = op;
        info.depth_stencil_state.back_stencil_state = op;
        info.depth_stencil_state.compare_mask = 0xFF;
        info.depth_stencil_state.write_mask = 0xFF;
        info.depth_stencil_state.enable_stencil_test = true;
      }
      info.target_info.color_target_descriptions = &target;
      info.target_info.num_color_targets = 1;
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
  }
  for (SDL_GPUShader* s : {scene_vertex, scene_fragment, compose_vertex, compose_fragment})
    if (s != nullptr) SDL_ReleaseGPUShader(device, s);

  bool made = shaders_made;
  if (!made) {
    fail("3D shaders");
  } else if (depth_format_ == SDL_GPU_TEXTUREFORMAT_INVALID) {
    error = "the graphics device has no depth buffer format";
    made = false;
  } else if (std::count(std::begin(scene_pipelines_), std::end(scene_pipelines_), nullptr) != 0 || !compose_pipeline_) {
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
    if (nearest_ == nullptr || atlas_ == nullptr || screen_ == nullptr) made = fail("3D textures");
  }
  if (!made) return false;  // What was made is let go by the destructor.
  log::writef(log::Level::Info, "3d", "the graphics hardware draws 3D scenes (%s, depth buffer format %d, %s)",
              SDL_GetGPUDeviceDriver(device), static_cast<int>(depth_format_),
              stencil_ ? "with polygon numbers" : "without polygon numbers");
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
  for (SDL_GPUTexture* t : {scene_target_, depth_})
    if (t != nullptr) SDL_ReleaseGPUTexture(device_, t);
  scene_target_ = depth_ = nullptr;
  scale_ = 0;
  scene_drawn_ = 0;
}

bool Hardware3D::make_targets(int scale) {
  release_targets();
  const int width = kScreenWidth * scale, height = kScreenHeight * scale;
  const SDL_GPUTextureUsageFlags colour = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
  bool made = true;
  const auto make = [&](SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage) {
    SDL_GPUTexture* t = make_texture(device_, format, usage, width, height);
    made = made && t != nullptr;
    return t;
  };
  scene_target_ = make(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, colour);
  depth_ = make(depth_format_, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
  for (Kept& k : kept_) k.texture = make(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, colour);
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

bool Hardware3D::ready(int scale) {
  if (device_ == nullptr) return false;
  scale = std::clamp(scale, 1, kLargestScale);
  return scale == scale_ || make_targets(scale);
}

bool Hardware3D::capture(const nds::HardwareScene& scene, const std::vector<std::uint32_t>& source,
                         std::uint64_t number, int scale) {
  if (!ready(scale)) return false;
  // In place of the oldest.
  Kept* oldest = &kept_[0];
  for (Kept& k : kept_)
    if (k.number < oldest->number) oldest = &k;
  oldest->number = 0;
  if (!run(&scene, source, nullptr, oldest->texture, 0)) return false;
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
  if (!run(scene, screen, source, outputs_[static_cast<std::size_t>(which & 1)], brightness)) return nullptr;
  failed = false;
  return shown_[static_cast<std::size_t>(which & 1)];
}

bool Hardware3D::run(const nds::HardwareScene* given, const std::vector<std::uint32_t>& screen, SDL_GPUTexture* source,
                     SDL_GPUTexture* target, std::uint16_t brightness) {
  if (screen.size() != std::size_t{kScreenWidth} * kScreenHeight) return false;
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

  // What goes up: the screen, the vertices and the new textures.
  const std::uint32_t vertex_bytes = new_scene ? static_cast<std::uint32_t>(scene.vertices.size() * sizeof(Scene::Vertex)) : 0;
  std::uint32_t bytes = kScreenBytes + aligned(vertex_bytes);
  for (std::uint32_t i : pending_)
    bytes += aligned(static_cast<std::uint32_t>(scene.textures[i].width * scene.textures[i].height * 4));
  if (!ensure_transfer(bytes) || (vertex_bytes != 0 && !ensure_vertex_buffer(vertex_bytes))) {
    log::warning("3d", std::string("cannot make the buffers for a 3D scene: ") + SDL_GetError());
    return false;
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
  SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
  SDL_GPUTextureTransferInfo from{};
  from.transfer_buffer = transfer_;
  SDL_GPUTextureRegion to{};
  to.texture = screen_;
  to.w = kScreenWidth;
  to.h = kScreenHeight;
  to.d = 1;
  SDL_UploadToGPUTexture(copy, &from, &to, true);
  if (vertex_bytes != 0) {
    const SDL_GPUTransferBufferLocation upload{transfer_, vertices_at};
    const SDL_GPUBufferRegion region{vertices_, 0, vertex_bytes};
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

  if (new_scene) {
    // 1. The scene, over the clear colour (times its alpha) at the clear depth.
    SDL_GPUColorTargetInfo colour{};
    colour.texture = scene_target_;
    const float a = scene.clear_colour[3];
    colour.clear_color = {scene.clear_colour[0] * a, scene.clear_colour[1] * a, scene.clear_colour[2] * a, a};
    colour.load_op = SDL_GPU_LOADOP_CLEAR;
    colour.store_op = SDL_GPU_STOREOP_STORE;
    colour.cycle = true;
    SDL_GPUDepthStencilTargetInfo depth{};
    depth.texture = depth_;
    depth.clear_depth = scene.clear_depth;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.stencil_load_op = SDL_GPU_LOADOP_CLEAR;
    depth.clear_stencil = 0;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.cycle = true;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &colour, 1, &depth);
    if (!scene.runs.empty()) {
      const SDL_GPUBufferBinding binding{vertices_, 0};
      SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
      const SDL_GPUTextureSamplerBinding atlas{atlas_, nearest_};
      SDL_BindGPUFragmentSamplers(pass, 0, &atlas, 1);
      SceneUniforms uniforms{};
      for (std::size_t i = 0; i < scene.toon.size(); ++i) std::memcpy(uniforms.toon[i], scene.toon[i].data(), sizeof uniforms.toon[i]);
      uniforms.options[0] = scene.alpha_threshold;
      uniforms.options[1] = scene.highlight ? 1.0f : 0.0f;
      for (const Scene::Run& run : scene.runs) {
        uniforms.options[2] = run.kind == Scene::Kind::Opaque ? 1.0f : 0.0f;
        SDL_BindGPUGraphicsPipeline(pass, scene_pipelines_[static_cast<int>(run.kind) * 2 + (run.depth_equal ? 1 : 0)]);
        if (stencil_) SDL_SetGPUStencilReference(pass, run.number);
        SDL_PushGPUFragmentUniformData(commands, 0, &uniforms, sizeof uniforms);
        SDL_DrawGPUPrimitives(pass, run.count, 1, run.first, 0);
      }
    }
    SDL_EndGPURenderPass(pass);
    scene_drawn_ = scene.serial;
  }

  // 2. The screen with the scene (or the kept capture) put in.
  SDL_GPUColorTargetInfo colour{};
  colour.texture = target;
  colour.load_op = SDL_GPU_LOADOP_DONT_CARE;
  colour.store_op = SDL_GPU_STOREOP_STORE;
  colour.cycle = true;
  SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &colour, 1, nullptr);
  SDL_BindGPUGraphicsPipeline(pass, compose_pipeline_);
  const std::array<SDL_GPUTextureSamplerBinding, 2> sources{{{screen_, nearest_}, {source != nullptr ? source : scene_target_, nearest_}}};
  SDL_BindGPUFragmentSamplers(pass, 0, sources.data(), 2);
  ComposeUniforms uniforms{};
  const unsigned direction = brightness >> 14;
  uniforms.brightness[0] = direction == 1 || direction == 2 ? static_cast<float>(direction) : 0.0f;
  uniforms.brightness[1] = static_cast<float>(std::min(brightness & 0x1F, 16)) / 16.0f;
  SDL_PushGPUFragmentUniformData(commands, 0, &uniforms, sizeof uniforms);
  SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
  SDL_EndGPURenderPass(pass);
  if (!SDL_SubmitGPUCommandBuffer(commands)) {
    log::warning("3d", std::string("cannot hand a 3D scene to the graphics hardware: ") + SDL_GetError());
    return false;
  }
  return true;
}

}  // namespace bd::app
