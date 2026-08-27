#include "hfior_wayland_client.h"

#include <SDL3/SDL.h>
#include <epoxy/gl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <time.h>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMouseRadiansPerCount = 0.0015;

enum class InputMode { kEager, kHfior };

struct Options {
  InputMode mode = InputMode::kEager;
  double seconds = 15.0;
  double warmup_seconds = 3.0;
  std::uint32_t objects = 65536;
  std::uint32_t reaction_objects = 0;
  std::uint32_t draw_repeats = 128;
  std::uint32_t requested_rate_hz = 8000;
  int width = 1280;
  int height = 720;
  std::filesystem::path output = "benchmark-output/game-run.csv";
};

struct Mat4 {
  std::array<float, 16> value{};
};

struct SceneObject {
  float x;
  float y;
  float z;
  float scale;
  float vx;
  float vy;
  float vz;
};

struct Camera {
  double yaw = 0.0;
  double pitch = 0.0;
};

struct FrameSample {
  std::uint64_t frame;
  std::uint64_t start_ns;
  std::uint64_t frame_ns;
  std::uint64_t input_records;
  std::uint64_t sdl_motion_events;
  std::uint64_t expensive_actions;
  std::uint64_t sample_age_ns;
  std::uint64_t producer_drops;
  std::uint64_t sequence_errors;
  std::uint32_t visible_objects;
};

struct CullResult {
  std::uint32_t visible = 0;
  double checksum = 0.0;
};

struct Renderer {
  GLuint program = 0;
  GLuint vao = 0;
  GLuint vertex_buffer = 0;
  GLuint index_buffer = 0;
  GLuint instance_buffer = 0;
  GLint mvp_location = -1;
  GLint time_location = -1;
  GLint visibility_location = -1;
};

std::uint64_t MonotonicNowNs() {
  timespec timestamp{};
  if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) {
    return 0;
  }
  return static_cast<std::uint64_t>(timestamp.tv_sec) * UINT64_C(1000000000) +
         static_cast<std::uint64_t>(timestamp.tv_nsec);
}

std::uint64_t ProcessCpuNowNs() {
  timespec timestamp{};
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &timestamp) != 0) {
    return 0;
  }
  return static_cast<std::uint64_t>(timestamp.tv_sec) * UINT64_C(1000000000) +
         static_cast<std::uint64_t>(timestamp.tv_nsec);
}

const char* ModeName(InputMode mode) {
  return mode == InputMode::kEager ? "eager-same-source"
                                   : "hfior-late-latch";
}

[[noreturn]] void Usage(const char* program, int exit_code) {
  std::fprintf(
      exit_code == 0 ? stdout : stderr,
      "Usage: %s --mode eager|hfior [options]\n"
      "  --seconds N          measured duration (default 15)\n"
      "  --warmup N           warmup duration (default 3)\n"
      "  --objects N          simulated/rendered scene objects (default 65536)\n"
      "  --reaction-objects N camera graph nodes per input action\n"
      "                       (default min(objects, 8192))\n"
      "  --draw-repeats N     repeated heavy geometry passes (default 128)\n"
      "  --requested-rate N   firmware/requested rate label (default 8000)\n"
      "  --width N            window width (default 1280)\n"
      "  --height N           window height (default 720)\n"
      "  --output PATH        raw frame CSV output\n"
      "\nMove the mouse continuously during warmup and measurement. Press Q to quit.\n",
      program);
  std::exit(exit_code);
}

std::uint64_t ParseUnsigned(const char* name, const char* value,
                            std::uint64_t minimum,
                            std::uint64_t maximum) {
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(value, &end, 10);
  if (!value[0] || !end || *end != '\0' || parsed < minimum ||
      parsed > maximum) {
    throw std::runtime_error(std::string("invalid ") + name + ": " + value);
  }
  return parsed;
}

double ParseDouble(const char* name, const char* value, double minimum,
                   double maximum) {
  char* end = nullptr;
  const double parsed = std::strtod(value, &end);
  if (!value[0] || !end || *end != '\0' || !std::isfinite(parsed) ||
      parsed < minimum || parsed > maximum) {
    throw std::runtime_error(std::string("invalid ") + name + ": " + value);
  }
  return parsed;
}

Options ParseOptions(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if (argument == "--help") {
      Usage(argv[0], 0);
    }
    if (index + 1 >= argc) {
      throw std::runtime_error("missing value after " + std::string(argument));
    }
    const char* value = argv[++index];
    if (argument == "--mode") {
      if (std::string_view(value) == "eager") {
        options.mode = InputMode::kEager;
      } else if (std::string_view(value) == "hfior") {
        options.mode = InputMode::kHfior;
      } else {
        throw std::runtime_error("--mode must be eager or hfior");
      }
    } else if (argument == "--seconds") {
      options.seconds = ParseDouble("--seconds", value, 1.0, 600.0);
    } else if (argument == "--warmup") {
      options.warmup_seconds = ParseDouble("--warmup", value, 0.0, 120.0);
    } else if (argument == "--objects") {
      options.objects = static_cast<std::uint32_t>(
          ParseUnsigned("--objects", value, 1024, 1000000));
    } else if (argument == "--reaction-objects") {
      options.reaction_objects = static_cast<std::uint32_t>(
          ParseUnsigned("--reaction-objects", value, 1, 1000000));
    } else if (argument == "--draw-repeats") {
      options.draw_repeats = static_cast<std::uint32_t>(
          ParseUnsigned("--draw-repeats", value, 1, 512));
    } else if (argument == "--requested-rate") {
      options.requested_rate_hz = static_cast<std::uint32_t>(
          ParseUnsigned("--requested-rate", value, 125, 64000));
    } else if (argument == "--width") {
      options.width = static_cast<int>(
          ParseUnsigned("--width", value, 320, 7680));
    } else if (argument == "--height") {
      options.height = static_cast<int>(
          ParseUnsigned("--height", value, 240, 4320));
    } else if (argument == "--output") {
      options.output = value;
    } else {
      throw std::runtime_error("unknown argument: " + std::string(argument));
    }
  }
  if (options.reaction_objects == 0) {
    options.reaction_objects = std::min(options.objects, UINT32_C(8192));
  } else if (options.reaction_objects > options.objects) {
    throw std::runtime_error("--reaction-objects cannot exceed --objects");
  }
  return options;
}

Mat4 Multiply(const Mat4& left, const Mat4& right) {
  Mat4 result;
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      float value = 0.0f;
      for (int inner = 0; inner < 4; ++inner) {
        value += left.value[inner * 4 + row] *
                 right.value[column * 4 + inner];
      }
      result.value[column * 4 + row] = value;
    }
  }
  return result;
}

Mat4 Perspective(float vertical_fov, float aspect, float near_plane,
                 float far_plane) {
  Mat4 result;
  const float inverse_tangent = 1.0f / std::tan(vertical_fov * 0.5f);
  result.value[0] = inverse_tangent / aspect;
  result.value[5] = inverse_tangent;
  result.value[10] = (far_plane + near_plane) / (near_plane - far_plane);
  result.value[11] = -1.0f;
  result.value[14] =
      (2.0f * far_plane * near_plane) / (near_plane - far_plane);
  return result;
}

Mat4 ViewMatrix(const Camera& camera) {
  const float cy = static_cast<float>(std::cos(camera.yaw));
  const float sy = static_cast<float>(std::sin(camera.yaw));
  const float cp = static_cast<float>(std::cos(camera.pitch));
  const float sp = static_cast<float>(std::sin(camera.pitch));
  Mat4 view{};
  view.value = {
      cy, -sy * sp, -sy * cp, 0.0f,
      0.0f, cp, -sp, 0.0f,
      sy, cy * sp, cy * cp, 0.0f,
      0.0f, 0.0f, 0.0f, 1.0f,
  };
  return view;
}

void ApplyMotion(Camera* camera, double dx, double dy) {
  camera->yaw += dx * kMouseRadiansPerCount;
  camera->pitch -= dy * kMouseRadiansPerCount;
  camera->pitch = std::clamp(camera->pitch, -1.45, 1.45);
  if (camera->yaw > kPi) {
    camera->yaw -= 2.0 * kPi;
  } else if (camera->yaw < -kPi) {
    camera->yaw += 2.0 * kPi;
  }
}

std::uint32_t Hash(std::uint32_t value) {
  value ^= value >> 16;
  value *= UINT32_C(0x7feb352d);
  value ^= value >> 15;
  value *= UINT32_C(0x846ca68b);
  return value ^ (value >> 16);
}

float UnitFloat(std::uint32_t value) {
  return static_cast<float>(Hash(value) & UINT32_C(0x00ffffff)) /
         static_cast<float>(UINT32_C(0x01000000));
}

std::vector<SceneObject> CreateScene(std::uint32_t count) {
  std::vector<SceneObject> scene;
  scene.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    const float vertical = UnitFloat(index * 9 + 1) * 2.0f - 1.0f;
    const float azimuth = UnitFloat(index * 9 + 2) *
                          static_cast<float>(2.0 * kPi);
    const float horizontal =
        std::sqrt(std::max(0.0f, 1.0f - vertical * vertical));
    const float radius = 12.0f + UnitFloat(index * 9 + 3) * 276.0f;
    scene.push_back({
        radius * horizontal * std::cos(azimuth), radius * vertical,
        radius * horizontal * std::sin(azimuth),
        0.3f + UnitFloat(index * 9 + 4) * 1.2f,
        (UnitFloat(index * 9 + 5) * 2.0f - 1.0f) * 0.18f,
        (UnitFloat(index * 9 + 6) * 2.0f - 1.0f) * 0.18f,
        (UnitFloat(index * 9 + 7) * 2.0f - 1.0f) * 0.18f,
    });
  }
  return scene;
}

void Simulate(std::vector<SceneObject>* scene, float delta_seconds) {
  for (SceneObject& object : *scene) {
    object.x += object.vx * delta_seconds;
    object.y += object.vy * delta_seconds;
    object.z += object.vz * delta_seconds;
  }
}

__attribute__((noinline)) CullResult CullScene(
    const std::vector<SceneObject>& scene, const Camera& camera,
    float aspect, std::vector<std::array<float, 4>>* visible_instances) {
  visible_instances->clear();
  const double cy = std::cos(camera.yaw);
  const double sy = std::sin(camera.yaw);
  const double cp = std::cos(camera.pitch);
  const double sp = std::sin(camera.pitch);
  const double forward_x = sy * cp;
  const double forward_y = sp;
  const double forward_z = -cy * cp;
  const double right_x = cy;
  const double right_z = sy;
  const double up_x = -sy * sp;
  const double up_y = cp;
  const double up_z = cy * sp;
  constexpr double tan_half_vertical = 0.7002075382;
  const double tan_half_horizontal = tan_half_vertical * aspect;
  double checksum = 0.0;

  for (std::size_t index = 0; index < scene.size(); ++index) {
    const SceneObject& object = scene[index];
    const double depth = object.x * forward_x + object.y * forward_y +
                         object.z * forward_z;
    const double horizontal = object.x * right_x + object.z * right_z;
    const double vertical = object.x * up_x + object.y * up_y +
                            object.z * up_z;
    const double radius = object.scale * 1.75;
    if (depth + radius >= 0.1 && depth - radius <= 320.0 &&
        std::abs(horizontal) <= depth * tan_half_horizontal + radius &&
        std::abs(vertical) <= depth * tan_half_vertical + radius) {
      visible_instances->push_back(
          {object.x, object.y, object.z, object.scale});
      checksum += depth * 0.000001 + static_cast<double>(index & 255) * 1e-9;
    }
  }
  return {static_cast<std::uint32_t>(visible_instances->size()), checksum};
}

// Models camera subscribers that games commonly update after input: aim state,
// visibility/LOD state, and camera-dependent gameplay queries. The final value
// reaches the renderer, so the compiler cannot discard intermediate work.
__attribute__((noinline)) double UpdateCameraReactionGraph(
    const std::vector<SceneObject>& scene, const Camera& camera,
    std::uint32_t reaction_objects, float aspect) {
  const double cy = std::cos(camera.yaw);
  const double sy = std::sin(camera.yaw);
  const double cp = std::cos(camera.pitch);
  const double sp = std::sin(camera.pitch);
  const double forward_x = sy * cp;
  const double forward_y = sp;
  const double forward_z = -cy * cp;
  const double right_x = cy;
  const double right_z = sy;
  const double up_x = -sy * sp;
  const double up_y = cp;
  const double up_z = cy * sp;
  constexpr double tan_half_vertical = 0.7002075382;
  const double tan_half_horizontal = tan_half_vertical * aspect;
  double checksum = 0.0;

  for (std::uint32_t index = 0; index < reaction_objects; ++index) {
    const SceneObject& object = scene[index];
    const double depth = object.x * forward_x + object.y * forward_y +
                         object.z * forward_z;
    const double horizontal = object.x * right_x + object.z * right_z;
    const double vertical = object.x * up_x + object.y * up_y +
                            object.z * up_z;
    const double radius = object.scale * 1.75;
    const bool visible =
        depth + radius >= 0.1 && depth - radius <= 320.0 &&
        std::abs(horizontal) <= depth * tan_half_horizontal + radius &&
        std::abs(vertical) <= depth * tan_half_vertical + radius;
    const double projected = radius / std::max(depth, 0.1);
    const unsigned lod = projected > 0.04 ? 0U : projected > 0.012 ? 1U : 2U;
    checksum += visible
                    ? depth * 1e-7 + horizontal * 1e-8 + vertical * 1e-8 +
                          static_cast<double>(lod) * 1e-6
                    : std::abs(horizontal) * 1e-10 +
                          static_cast<double>((index ^ lod) & 31U) * 1e-9;
  }
  return checksum;
}

GLuint CompileShader(GLenum type, const char* source) {
  const GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint compiled = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled == GL_TRUE) {
    return shader;
  }
  std::array<char, 4096> log{};
  glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), nullptr,
                     log.data());
  glDeleteShader(shader);
  throw std::runtime_error(std::string("shader compilation failed: ") +
                           log.data());
}

Renderer CreateRenderer(std::uint32_t max_instances) {
  static constexpr char kVertexShader[] = R"glsl(
#version 450 core
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_instance;
uniform mat4 u_mvp;
uniform float u_time;
out vec3 normal;
out vec3 world_position;
flat out uint instance_id;
void main() {
  float phase = u_time * 0.35 + float(gl_InstanceID % 97) * 0.061;
  mat2 rotation = mat2(cos(phase), -sin(phase), sin(phase), cos(phase));
  vec3 local = in_position * in_instance.w;
  local.xz = rotation * local.xz;
  vec3 world = in_instance.xyz + local;
  normal = normalize(vec3(rotation * in_normal.xz, in_normal.y).xzy);
  world_position = world;
  instance_id = uint(gl_InstanceID);
  gl_Position = u_mvp * vec4(world, 1.0);
}
)glsl";
  static constexpr char kFragmentShader[] = R"glsl(
#version 450 core
in vec3 normal;
in vec3 world_position;
flat in uint instance_id;
uniform float u_visibility;
out vec4 out_color;
void main() {
  vec3 n = normalize(normal);
  float energy = max(dot(n, normalize(vec3(0.4, 0.8, 0.3))), 0.0);
  float detail = 0.0;
  vec3 p = world_position * 0.035 + float(instance_id & 31u);
  for (int i = 0; i < 12; ++i) {
    p = abs(p) / max(dot(p, p), 0.18) - vec3(0.72, 0.64, 0.58);
    detail += exp(-abs(dot(p, p) - 0.7)) * 0.055;
  }
  vec3 base = 0.32 + 0.28 * cos(vec3(0.1, 2.2, 4.3) +
                                  float(instance_id % 251u) * 0.07);
  float fog = clamp(1.0 - length(world_position) / 330.0, 0.08, 1.0);
  float exposure = 0.92 + min(u_visibility, 1.0) * 0.08;
  out_color = vec4(base * (0.16 + energy * 0.84 + detail) * fog * exposure,
                   1.0);
}
)glsl";

  const GLuint vertex = CompileShader(GL_VERTEX_SHADER, kVertexShader);
  const GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, kFragmentShader);
  Renderer renderer;
  renderer.program = glCreateProgram();
  glAttachShader(renderer.program, vertex);
  glAttachShader(renderer.program, fragment);
  glLinkProgram(renderer.program);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  GLint linked = GL_FALSE;
  glGetProgramiv(renderer.program, GL_LINK_STATUS, &linked);
  if (linked != GL_TRUE) {
    std::array<char, 4096> log{};
    glGetProgramInfoLog(renderer.program, static_cast<GLsizei>(log.size()),
                        nullptr, log.data());
    throw std::runtime_error(std::string("shader link failed: ") + log.data());
  }

  struct Vertex {
    float position[3];
    float normal[3];
  };
  static constexpr std::array<Vertex, 24> kVertices = {{
      {{-1, -1, 1}, {0, 0, 1}}, {{1, -1, 1}, {0, 0, 1}},
      {{1, 1, 1}, {0, 0, 1}}, {{-1, 1, 1}, {0, 0, 1}},
      {{1, -1, -1}, {0, 0, -1}}, {{-1, -1, -1}, {0, 0, -1}},
      {{-1, 1, -1}, {0, 0, -1}}, {{1, 1, -1}, {0, 0, -1}},
      {{-1, -1, -1}, {-1, 0, 0}}, {{-1, -1, 1}, {-1, 0, 0}},
      {{-1, 1, 1}, {-1, 0, 0}}, {{-1, 1, -1}, {-1, 0, 0}},
      {{1, -1, 1}, {1, 0, 0}}, {{1, -1, -1}, {1, 0, 0}},
      {{1, 1, -1}, {1, 0, 0}}, {{1, 1, 1}, {1, 0, 0}},
      {{-1, 1, 1}, {0, 1, 0}}, {{1, 1, 1}, {0, 1, 0}},
      {{1, 1, -1}, {0, 1, 0}}, {{-1, 1, -1}, {0, 1, 0}},
      {{-1, -1, -1}, {0, -1, 0}}, {{1, -1, -1}, {0, -1, 0}},
      {{1, -1, 1}, {0, -1, 0}}, {{-1, -1, 1}, {0, -1, 0}},
  }};
  static constexpr std::array<std::uint16_t, 36> kIndices = {
      0, 1, 2, 2, 3, 0, 4, 5, 6, 6, 7, 4, 8, 9, 10, 10, 11, 8,
      12, 13, 14, 14, 15, 12, 16, 17, 18, 18, 19, 16, 20, 21, 22,
      22, 23, 20};

  glGenVertexArrays(1, &renderer.vao);
  glBindVertexArray(renderer.vao);
  glGenBuffers(1, &renderer.vertex_buffer);
  glBindBuffer(GL_ARRAY_BUFFER, renderer.vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, sizeof(kVertices), kVertices.data(),
               GL_STATIC_DRAW);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), nullptr);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                        reinterpret_cast<void*>(sizeof(float) * 3));
  glEnableVertexAttribArray(1);
  glGenBuffers(1, &renderer.index_buffer);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, renderer.index_buffer);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(kIndices), kIndices.data(),
               GL_STATIC_DRAW);
  glGenBuffers(1, &renderer.instance_buffer);
  glBindBuffer(GL_ARRAY_BUFFER, renderer.instance_buffer);
  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(max_instances) * sizeof(float) * 4,
               nullptr, GL_STREAM_DRAW);
  glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(float) * 4, nullptr);
  glEnableVertexAttribArray(2);
  glVertexAttribDivisor(2, 1);
  glBindVertexArray(0);

  renderer.mvp_location = glGetUniformLocation(renderer.program, "u_mvp");
  renderer.time_location = glGetUniformLocation(renderer.program, "u_time");
  renderer.visibility_location =
      glGetUniformLocation(renderer.program, "u_visibility");
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glEnable(GL_CULL_FACE);
  glCullFace(GL_BACK);
  return renderer;
}

void DestroyRenderer(Renderer* renderer) {
  glDeleteBuffers(1, &renderer->instance_buffer);
  glDeleteBuffers(1, &renderer->index_buffer);
  glDeleteBuffers(1, &renderer->vertex_buffer);
  glDeleteVertexArrays(1, &renderer->vao);
  glDeleteProgram(renderer->program);
}

void Render(const Renderer& renderer,
            const std::vector<std::array<float, 4>>& visible_instances,
            const Camera& camera, int width, int height,
            std::uint32_t total_objects, std::uint32_t draw_repeats,
            float elapsed_seconds) {
  glViewport(0, 0, width, height);
  glClearColor(0.004f, 0.007f, 0.016f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glUseProgram(renderer.program);
  const float aspect = static_cast<float>(width) / static_cast<float>(height);
  const Mat4 mvp = Multiply(Perspective(1.22173048f, aspect, 0.1f, 330.0f),
                            ViewMatrix(camera));
  glUniformMatrix4fv(renderer.mvp_location, 1, GL_FALSE, mvp.value.data());
  glUniform1f(renderer.time_location, elapsed_seconds);
  glUniform1f(renderer.visibility_location,
              static_cast<float>(visible_instances.size()) /
                  static_cast<float>(total_objects));
  glBindVertexArray(renderer.vao);
  glBindBuffer(GL_ARRAY_BUFFER, renderer.instance_buffer);
  glBufferSubData(GL_ARRAY_BUFFER, 0,
                  static_cast<GLsizeiptr>(visible_instances.size()) *
                      sizeof(visible_instances.front()),
                  visible_instances.data());
  for (std::uint32_t pass = 0; pass < draw_repeats; ++pass) {
    glDrawElementsInstanced(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr,
                            static_cast<GLsizei>(visible_instances.size()));
  }
  glBindVertexArray(0);
  glFinish();
}

double Percentile(const std::vector<std::uint64_t>& values, double percentile) {
  if (values.empty()) return 0.0;
  std::vector<std::uint64_t> sorted = values;
  std::sort(sorted.begin(), sorted.end());
  const double position = percentile * static_cast<double>(sorted.size() - 1);
  const std::size_t lower = static_cast<std::size_t>(position);
  const std::size_t upper = std::min(lower + 1, sorted.size() - 1);
  const double fraction = position - static_cast<double>(lower);
  return static_cast<double>(sorted[lower]) * (1.0 - fraction) +
         static_cast<double>(sorted[upper]) * fraction;
}

double LowFps(const std::vector<std::uint64_t>& frame_times,
              double slow_fraction) {
  if (frame_times.empty()) return 0.0;
  std::vector<std::uint64_t> sorted = frame_times;
  std::sort(sorted.begin(), sorted.end(), std::greater<>());
  const std::size_t count = std::max<std::size_t>(
      1, static_cast<std::size_t>(std::ceil(sorted.size() * slow_fraction)));
  const long double total = std::accumulate(
      sorted.begin(), sorted.begin() + count, static_cast<long double>(0));
  const long double mean = total / static_cast<long double>(count);
  return mean > 0 ? static_cast<double>(1.0e9L / mean) : 0.0;
}

std::string JsonEscape(std::string_view input) {
  std::string output;
  for (const char character : input) {
    if (character == '"' || character == '\\') output.push_back('\\');
    if (character == '\n') {
      output += "\\n";
    } else {
      output.push_back(character);
    }
  }
  return output;
}

bool WriteResults(const Options& options,
                  const std::vector<FrameSample>& samples,
                  std::uint64_t cpu_ns, const rusage& usage_start,
                  const rusage& usage_end, std::string_view video_driver,
                  std::string_view gl_vendor, std::string_view gl_renderer,
                  std::string_view gl_version, std::string_view device,
                  int pixel_width, int pixel_height, bool integrity_valid,
                  std::string_view failure) {
  if (!options.output.parent_path().empty()) {
    std::filesystem::create_directories(options.output.parent_path());
  }
  std::ofstream csv(options.output);
  if (!csv) {
    throw std::runtime_error("could not open output CSV: " +
                             options.output.string());
  }
  csv << "frame,start_ns,frame_ns,input_records,sdl_motion_events,"
         "expensive_actions,sample_age_ns,producer_drops,sequence_errors,"
         "visible_objects\n";
  for (const FrameSample& sample : samples) {
    csv << sample.frame << ',' << sample.start_ns << ',' << sample.frame_ns
        << ',' << sample.input_records << ',' << sample.sdl_motion_events
        << ',' << sample.expensive_actions << ',' << sample.sample_age_ns
        << ',' << sample.producer_drops << ',' << sample.sequence_errors
        << ',' << sample.visible_objects << '\n';
  }

  std::vector<std::uint64_t> frame_times;
  std::vector<std::uint64_t> sample_ages;
  frame_times.reserve(samples.size());
  sample_ages.reserve(samples.size());
  std::uint64_t records = 0;
  std::uint64_t sdl_events = 0;
  std::uint64_t actions = 0;
  std::uint64_t drops = 0;
  std::uint64_t sequence_errors = 0;
  for (const FrameSample& sample : samples) {
    frame_times.push_back(sample.frame_ns);
    if (sample.sample_age_ns != 0) sample_ages.push_back(sample.sample_age_ns);
    records += sample.input_records;
    sdl_events += sample.sdl_motion_events;
    actions += sample.expensive_actions;
    drops = std::max(drops, sample.producer_drops);
    sequence_errors = std::max(sequence_errors, sample.sequence_errors);
  }
  const long double total_frame_ns = std::accumulate(
      frame_times.begin(), frame_times.end(), static_cast<long double>(0));
  const double measured_seconds = static_cast<double>(total_frame_ns / 1.0e9L);
  const double fps = measured_seconds > 0
                         ? static_cast<double>(samples.size()) / measured_seconds
                         : 0.0;
  const double records_per_second =
      measured_seconds > 0 ? static_cast<double>(records) / measured_seconds
                           : 0.0;
  const double actions_per_second =
      measured_seconds > 0 ? static_cast<double>(actions) / measured_seconds
                           : 0.0;
  const bool high_rate_valid =
      options.requested_rate_hz < 5500 || records_per_second >= 5500.0;
  const bool valid = integrity_valid && high_rate_valid && !samples.empty();
  const long voluntary_switches =
      usage_end.ru_nvcsw - usage_start.ru_nvcsw;
  const long involuntary_switches =
      usage_end.ru_nivcsw - usage_start.ru_nivcsw;

  const std::filesystem::path summary_path =
      options.output.string() + ".summary.json";
  std::ofstream summary(summary_path);
  if (!summary) {
    throw std::runtime_error("could not open summary JSON: " +
                             summary_path.string());
  }
  struct utsname system_info {};
  (void)uname(&system_info);
  summary << std::fixed << std::setprecision(3)
          << "{\n"
          << "  \"schema\": 1,\n"
          << "  \"benchmark\": \"hfior-game\",\n"
          << "  \"evidence_class\": \"interactive-physical-candidate\",\n"
          << "  \"mode\": \"" << ModeName(options.mode) << "\",\n"
          << "  \"valid\": " << (valid ? "true" : "false") << ",\n"
          << "  \"integrity_valid\": "
          << (integrity_valid ? "true" : "false") << ",\n"
          << "  \"high_rate_valid\": "
          << (high_rate_valid ? "true" : "false") << ",\n"
          << "  \"failure\": \"" << JsonEscape(failure) << "\",\n"
          << "  \"requested_rate_hz\": " << options.requested_rate_hz
          << ",\n"
          << "  \"observed_input_records_per_s\": " << records_per_second
          << ",\n"
          << "  \"input_source\": \"authorized-hfior-ring\",\n"
          << "  \"device\": \"" << JsonEscape(device) << "\",\n"
          << "  \"frames\": " << samples.size() << ",\n"
          << "  \"measured_seconds\": " << measured_seconds << ",\n"
          << "  \"average_fps\": " << fps << ",\n"
          << "  \"one_percent_low_fps\": " << LowFps(frame_times, 0.01)
          << ",\n"
          << "  \"zero_point_one_percent_low_fps\": "
          << LowFps(frame_times, 0.001) << ",\n"
          << "  \"frametime_p50_ns\": " << Percentile(frame_times, 0.50)
          << ",\n"
          << "  \"frametime_p95_ns\": " << Percentile(frame_times, 0.95)
          << ",\n"
          << "  \"frametime_p99_ns\": " << Percentile(frame_times, 0.99)
          << ",\n"
          << "  \"frametime_p99_9_ns\": "
          << Percentile(frame_times, 0.999) << ",\n"
          << "  \"frametime_max_ns\": "
          << (frame_times.empty()
                  ? 0
                  : *std::max_element(frame_times.begin(), frame_times.end()))
          << ",\n"
          << "  \"sample_age_p50_ns\": " << Percentile(sample_ages, 0.50)
          << ",\n"
          << "  \"sample_age_p95_ns\": " << Percentile(sample_ages, 0.95)
          << ",\n"
          << "  \"sample_age_p99_ns\": " << Percentile(sample_ages, 0.99)
          << ",\n"
          << "  \"sample_age_p99_9_ns\": "
          << Percentile(sample_ages, 0.999) << ",\n"
          << "  \"expensive_actions_per_s\": " << actions_per_second
          << ",\n"
          << "  \"sdl_motion_events\": " << sdl_events << ",\n"
          << "  \"producer_drops\": " << drops << ",\n"
          << "  \"sequence_errors\": " << sequence_errors << ",\n"
          << "  \"process_cpu_ms_per_s\": "
          << (measured_seconds > 0
                  ? static_cast<double>(cpu_ns) / 1.0e6 / measured_seconds
                  : 0.0)
          << ",\n"
          << "  \"voluntary_context_switches_per_s\": "
          << (measured_seconds > 0 ? voluntary_switches / measured_seconds
                                   : 0.0)
          << ",\n"
          << "  \"involuntary_context_switches_per_s\": "
          << (measured_seconds > 0 ? involuntary_switches / measured_seconds
                                   : 0.0)
          << ",\n"
          << "  \"objects\": " << options.objects << ",\n"
          << "  \"reaction_objects\": " << options.reaction_objects << ",\n"
          << "  \"draw_repeats\": " << options.draw_repeats << ",\n"
          << "  \"window_width\": " << pixel_width << ",\n"
          << "  \"window_height\": " << pixel_height << ",\n"
          << "  \"requested_window_width\": " << options.width << ",\n"
          << "  \"requested_window_height\": " << options.height << ",\n"
          << "  \"video_driver\": \"" << JsonEscape(video_driver)
          << "\",\n"
          << "  \"gl_vendor\": \"" << JsonEscape(gl_vendor) << "\",\n"
          << "  \"gl_renderer\": \"" << JsonEscape(gl_renderer)
          << "\",\n"
          << "  \"gl_version\": \"" << JsonEscape(gl_version) << "\",\n"
          << "  \"kernel\": \"" << JsonEscape(system_info.release)
          << "\",\n"
          << "  \"raw_csv\": \"" << JsonEscape(options.output.string())
          << "\"\n"
          << "}\n";

  std::cout << std::fixed << std::setprecision(1)
            << "\nResult: " << (valid ? "VALID" : "INVALID") << '\n'
            << "  mode:                 " << ModeName(options.mode) << '\n'
            << "  observed input:       " << std::setprecision(1)
            << records_per_second << " records/s\n"
            << "  expensive actions:    " << actions_per_second
            << " actions/s\n"
            << "  average FPS:           " << fps << '\n'
            << "  1% low FPS:            " << LowFps(frame_times, 0.01)
            << '\n'
            << "  frametime p99:         "
            << Percentile(frame_times, 0.99) / 1.0e6 << " ms\n"
            << "  useful sample age p99: "
            << Percentile(sample_ages, 0.99) / 1000.0 << " us\n"
            << "  drops / seq errors:    " << drops << " / "
            << sequence_errors << '\n'
            << "  raw:                   " << options.output << '\n'
            << "  summary:               " << summary_path << '\n';
  return valid;
}

int Run(const Options& options) {
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
    throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
  }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_Window* window = SDL_CreateWindow(
      "HFIOR 3D benchmark", options.width, options.height,
      SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window) {
    throw std::runtime_error(std::string("SDL_CreateWindow failed: ") +
                             SDL_GetError());
  }
  SDL_GLContext context = SDL_GL_CreateContext(window);
  if (!context) {
    throw std::runtime_error(std::string("SDL_GL_CreateContext failed: ") +
                             SDL_GetError());
  }
  if (!SDL_GL_MakeCurrent(window, context)) {
    throw std::runtime_error(std::string("SDL_GL_MakeCurrent failed: ") +
                             SDL_GetError());
  }
  (void)SDL_GL_SetSwapInterval(0);
  SDL_RaiseWindow(window);

  const char* video_driver = SDL_GetCurrentVideoDriver();
  if (!video_driver) video_driver = "unknown";
  if (std::string_view(video_driver) != "wayland") {
    throw std::runtime_error(
        "HFIOR mode requires SDL_VIDEODRIVER=wayland");
  }

  const char* gl_vendor =
      reinterpret_cast<const char*>(glGetString(GL_VENDOR));
  const char* gl_renderer =
      reinterpret_cast<const char*>(glGetString(GL_RENDERER));
  const char* gl_version =
      reinterpret_cast<const char*>(glGetString(GL_VERSION));
  if (!gl_vendor) gl_vendor = "unknown";
  if (!gl_renderer) gl_renderer = "unknown";
  if (!gl_version) gl_version = "unknown";
  std::cout << "HFIOR game benchmark\n"
            << "  mode:       " << ModeName(options.mode) << '\n'
            << "  renderer:   " << gl_renderer << '\n'
            << "  workload:   " << options.objects << " scene objects, "
            << options.reaction_objects << " camera graph nodes/action, "
            << options.draw_repeats << " geometry passes\n"
            << "  run:        " << options.warmup_seconds << " s warmup + "
            << options.seconds << " s measured\n"
            << "  controls:   move continuously, Q quits\n";

  Renderer renderer = CreateRenderer(options.objects);
  std::vector<SceneObject> scene = CreateScene(options.objects);
  std::vector<std::array<float, 4>> visible_instances;
  visible_instances.reserve(options.objects);
  constexpr std::size_t kMaximumRingRecords = 4096;
  std::array<hfior_game_motion, kMaximumRingRecords> early_motions{};
  std::array<hfior_game_motion, kMaximumRingRecords> late_motions{};
  Camera camera;

  hfior_game_client* hfior = hfior_game_client_create();
  if (!hfior) throw std::runtime_error("could not allocate HFIOR client");
  const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
  void* wayland_display = SDL_GetPointerProperty(
      properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
  void* wayland_surface = SDL_GetPointerProperty(
      properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
  if (!wayland_display || !wayland_surface) {
    throw std::runtime_error("SDL did not expose Wayland window handles");
  }

  bool running = true;
  bool captured = SDL_SetWindowRelativeMouseMode(window, true);
  bool integrity_valid = true;
  std::string failure;
  std::uint64_t last_connect_attempt_ns = 0;
  std::uint64_t ready_since_ns = 0;
  std::uint64_t measurement_start_ns = 0;
  std::uint64_t process_cpu_start_ns = 0;
  std::uint64_t previous_frame_start_ns = SDL_GetTicksNS();
  std::uint64_t frame_index = 0;
  bool measurement_started = false;
  rusage usage_start{};
  rusage usage_end{};
  std::vector<FrameSample> samples;
  samples.reserve(static_cast<std::size_t>(options.seconds * 1000.0));
  int last_pixel_width = options.width;
  int last_pixel_height = options.height;

  while (running) {
    const std::uint64_t frame_start_ns = SDL_GetTicksNS();
    const double frame_delta = std::clamp(
        static_cast<double>(frame_start_ns - previous_frame_start_ns) / 1.0e9,
        0.0, 0.05);
    previous_frame_start_ns = frame_start_ns;
    std::uint64_t sdl_motion_events = 0;

    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_EVENT_QUIT) {
        running = false;
      } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                 event.key.scancode == SDL_SCANCODE_Q) {
        running = false;
      } else if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
        captured = SDL_SetWindowRelativeMouseMode(window, true);
      } else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        captured = false;
        if (measurement_started) {
          integrity_valid = false;
          failure = "window focus was lost during measurement";
          running = false;
        }
      } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !captured) {
        captured = SDL_SetWindowRelativeMouseMode(window, true);
      } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
        ++sdl_motion_events;
      }
    }
    if (!running) break;

    if (captured && !hfior_game_client_active(hfior) &&
        frame_start_ns - last_connect_attempt_ns >= UINT64_C(250000000)) {
      last_connect_attempt_ns = frame_start_ns;
      if (hfior_game_client_connect(hfior, wayland_display, wayland_surface)) {
        std::cout << "  HFIOR active: " << hfior_game_client_device(hfior)
                  << '\n';
      }
    }

    const bool ready = captured && hfior_game_client_active(hfior);
    if (ready) {
      if (ready_since_ns == 0) ready_since_ns = frame_start_ns;
    } else {
      ready_since_ns = 0;
    }
    if (!measurement_started && ready_since_ns != 0 &&
        static_cast<double>(frame_start_ns - ready_since_ns) / 1.0e9 >=
            options.warmup_seconds) {
      measurement_started = true;
      measurement_start_ns = frame_start_ns;
      process_cpu_start_ns = ProcessCpuNowNs();
      (void)getrusage(RUSAGE_SELF, &usage_start);
      std::cout << "  measurement started\n";
    }

    std::uint64_t input_records = 0;
    std::uint64_t newest_input_timestamp_ns = 0;
    std::uint64_t producer_drops = 0;
    std::uint64_t sequence_errors = 0;
    hfior_game_batch early{};
    hfior_game_batch late{};
    if (hfior_game_client_active(hfior)) {
      if (!hfior_game_client_read(hfior, true, &early,
                                  early_motions.data(),
                                  early_motions.size())) {
        if (measurement_started) {
          integrity_valid = false;
          failure = hfior_game_client_error(hfior);
          break;
        }
      } else {
        if (options.mode == InputMode::kHfior) {
          for (std::size_t index = 0; index < early.records; ++index) {
            const hfior_game_motion& motion = early_motions[index];
            ApplyMotion(&camera, motion.dx, motion.dy);
          }
        }
        input_records += early.records;
        newest_input_timestamp_ns = early.newest_timestamp_ns;
        producer_drops = early.producer_drops;
        sequence_errors = early.sequence_errors;
      }
    }

    Simulate(&scene, static_cast<float>(frame_delta));
    int pixel_width = options.width;
    int pixel_height = options.height;
    (void)SDL_GetWindowSizeInPixels(window, &pixel_width, &pixel_height);
    if (pixel_width <= 0 || pixel_height <= 0) continue;
    last_pixel_width = pixel_width;
    last_pixel_height = pixel_height;
    const float aspect =
        static_cast<float>(pixel_width) / static_cast<float>(pixel_height);
    std::uint64_t expensive_actions = 0;
    double reaction_checksum = 0.0;

    if (hfior_game_client_active(hfior) &&
        !hfior_game_client_read(hfior, false, &late, late_motions.data(),
                                late_motions.size())) {
      integrity_valid = false;
      failure = hfior_game_client_error(hfior);
      break;
    }
    input_records += late.records;
    if (late.newest_timestamp_ns != 0) {
      newest_input_timestamp_ns = late.newest_timestamp_ns;
    }
    producer_drops = std::max(producer_drops, late.producer_drops);
    sequence_errors = std::max(sequence_errors, late.sequence_errors);

    if (options.mode == InputMode::kEager) {
      for (std::size_t index = 0; index < early.records; ++index) {
        const hfior_game_motion& motion = early_motions[index];
        ApplyMotion(&camera, motion.dx, motion.dy);
        reaction_checksum += UpdateCameraReactionGraph(
            scene, camera, options.reaction_objects, aspect);
        ++expensive_actions;
        newest_input_timestamp_ns = motion.timestamp_ns;
      }
      for (std::size_t index = 0; index < late.records; ++index) {
        const hfior_game_motion& motion = late_motions[index];
        ApplyMotion(&camera, motion.dx, motion.dy);
        reaction_checksum += UpdateCameraReactionGraph(
            scene, camera, options.reaction_objects, aspect);
        ++expensive_actions;
        newest_input_timestamp_ns = motion.timestamp_ns;
      }
      if (input_records == 0) {
        reaction_checksum = UpdateCameraReactionGraph(
            scene, camera, options.reaction_objects, aspect);
        ++expensive_actions;
      }
    } else if (hfior_game_client_active(hfior)) {
      for (std::size_t index = 0; index < late.records; ++index) {
        const hfior_game_motion& motion = late_motions[index];
        ApplyMotion(&camera, motion.dx, motion.dy);
      }
      reaction_checksum = UpdateCameraReactionGraph(
          scene, camera, options.reaction_objects, aspect);
      ++expensive_actions;
    } else {
      reaction_checksum = UpdateCameraReactionGraph(
          scene, camera, options.reaction_objects, aspect);
      ++expensive_actions;
    }

    // This is the heavy game baseline. Both policies always simulate, cull,
    // upload, and render the complete scene once per frame.
    const CullResult cull =
        CullScene(scene, camera, aspect, &visible_instances);

    const std::uint64_t useful_boundary_ns = MonotonicNowNs();
    const std::uint64_t sample_age_ns =
        newest_input_timestamp_ns != 0 &&
                useful_boundary_ns >= newest_input_timestamp_ns
            ? useful_boundary_ns - newest_input_timestamp_ns
            : 0;
    const float elapsed_seconds = static_cast<float>(frame_start_ns / 1.0e9);
    Render(renderer, visible_instances, camera, pixel_width, pixel_height,
           options.objects, options.draw_repeats,
           elapsed_seconds +
               static_cast<float>((cull.checksum + reaction_checksum) * 1e-8));
    if (!SDL_GL_SwapWindow(window)) {
      integrity_valid = false;
      failure = std::string("SDL_GL_SwapWindow failed: ") + SDL_GetError();
      break;
    }
    if (hfior_game_client_active(hfior) &&
        !hfior_game_client_acknowledge(hfior)) {
      integrity_valid = false;
      failure = hfior_game_client_error(hfior);
      break;
    }
    const std::uint64_t frame_end_ns = SDL_GetTicksNS();

    if (measurement_started) {
      samples.push_back({frame_index++, frame_start_ns,
                         frame_end_ns - frame_start_ns, input_records,
                         sdl_motion_events, expensive_actions,
                         sample_age_ns, producer_drops, sequence_errors,
                         cull.visible});
      const double measured_elapsed =
          static_cast<double>(frame_end_ns - measurement_start_ns) / 1.0e9;
      if (measured_elapsed >= options.seconds) running = false;
    }

    if ((frame_index & 127U) == 0U) {
      char title[256];
      std::snprintf(title, sizeof(title),
                    "HFIOR 3D benchmark | %s | %s | move mouse continuously",
                    ModeName(options.mode),
                    measurement_started ? "MEASURING" : "WARMUP / WAITING");
      (void)SDL_SetWindowTitle(window, title);
    }
  }

  (void)getrusage(RUSAGE_SELF, &usage_end);
  const std::uint64_t process_cpu_end_ns = ProcessCpuNowNs();
  const std::uint64_t cpu_ns =
      process_cpu_end_ns >= process_cpu_start_ns && process_cpu_start_ns != 0
          ? process_cpu_end_ns - process_cpu_start_ns
          : 0;
  const std::string device = hfior_game_client_device(hfior);
  const bool result_valid = WriteResults(
      options, samples, cpu_ns, usage_start, usage_end, video_driver,
      gl_vendor, gl_renderer, gl_version, device, last_pixel_width,
      last_pixel_height, integrity_valid, failure);

  hfior_game_client_destroy(hfior);
  DestroyRenderer(&renderer);
  SDL_GL_DestroyContext(context);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return result_valid ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return Run(ParseOptions(argc, argv));
  } catch (const std::exception& error) {
    std::fprintf(stderr, "hfior-game: %s\n", error.what());
    return 1;
  }
}
