#include "app/child_process.h"

namespace bd::app {

ChildProcess::~ChildProcess() {
  if (process_ == nullptr) return;
  if (!ended_) {
    SDL_KillProcess(process_, true);
    int code = 0;
    SDL_WaitProcess(process_, true, &code);
  }
  SDL_DestroyProcess(process_);
}

bool ChildProcess::start(const std::vector<std::string>& args, bool input, std::string& error) {
  if (process_ != nullptr || args.empty()) {
    error = "nothing to run";
    return false;
  }
  std::vector<const char*> argv;
  for (const std::string& a : args) argv.push_back(a.c_str());
  argv.push_back(nullptr);
  const SDL_PropertiesID props = SDL_CreateProperties();
  SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
  SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER,
                        input ? SDL_PROCESS_STDIO_APP : SDL_PROCESS_STDIO_NULL);
  SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
  SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
#if defined(_WIN32)
  // No console window: SDL adds CREATE_NO_WINDOW for these. The pipes set
  // above stay.
  SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
#endif
  process_ = SDL_CreateProcessWithProperties(props);
  SDL_DestroyProperties(props);
  if (process_ == nullptr) {
    error = SDL_GetError();
    return false;
  }
  ended_ = false;
  exit_code_ = -1;
  partial_.clear();
  return true;
}

void ChildProcess::read_output(const std::function<void(std::string_view)>& line) {
  SDL_IOStream* out = SDL_GetProcessOutput(process_);
  if (out == nullptr) return;
  char buffer[4096];
  for (;;) {
    const std::size_t n = SDL_ReadIO(out, buffer, sizeof buffer);
    if (n == 0) break;  // Nothing more now, or the end.
    partial_.append(buffer, n);
    std::size_t start = 0;
    for (std::size_t end; (end = partial_.find('\n', start)) != std::string::npos; start = end + 1) {
      std::string_view text(partial_.data() + start, end - start);
      if (!text.empty() && text.back() == '\r') text.remove_suffix(1);
      line(text);
    }
    partial_.erase(0, start);
  }
}

void ChildProcess::poll(const std::function<void(std::string_view)>& line) {
  if (process_ == nullptr || ended_) return;
  read_output(line);
  int code = 0;
  // Whether it has ended, without waiting. Programs it started may still
  // hold its output open; its end is what counts.
  if (SDL_WaitProcess(process_, false, &code)) {
    read_output(line);
    if (!partial_.empty()) {
      line(partial_);
      partial_.clear();
    }
    ended_ = true;
    exit_code_ = code;
  }
}

void ChildProcess::close_input() {
  if (process_ == nullptr) return;
  if (SDL_IOStream* in = SDL_GetProcessInput(process_)) SDL_CloseIO(in);
}

void ChildProcess::kill() {
  if (process_ != nullptr && !ended_) SDL_KillProcess(process_, true);
}

bool start_detached(const std::vector<std::string>& args, std::string& error) {
  if (args.empty()) {
    error = "nothing to run";
    return false;
  }
  std::vector<const char*> argv;
  for (const std::string& a : args) argv.push_back(a.c_str());
  argv.push_back(nullptr);
  const SDL_PropertiesID props = SDL_CreateProperties();
  SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
  SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
  SDL_Process* process = SDL_CreateProcessWithProperties(props);
  SDL_DestroyProperties(props);
  if (process == nullptr) {
    error = SDL_GetError();
    return false;
  }
  // Only the handle goes; the program goes on.
  SDL_DestroyProcess(process);
  return true;
}

ProgramOutput run_program(const std::vector<std::string>& args, std::uint32_t timeout_ms) {
  ProgramOutput result;
  ChildProcess child;
  std::string error;
  if (!child.start(args, false, error)) return result;
  result.started = true;
  const std::uint64_t until = SDL_GetTicks() + timeout_ms;
  const auto take = [&](std::string_view text) {
    result.output.append(text);
    result.output += '\n';
  };
  while (!child.ended()) {
    child.poll(take);
    if (child.ended()) break;
    if (SDL_GetTicks() >= until) {
      child.kill();
      for (int i = 0; i < 100 && !child.ended(); ++i) {
        SDL_Delay(10);
        child.poll(take);
      }
      return result;
    }
    SDL_Delay(5);
  }
  result.exit_code = child.exit_code();
  return result;
}

}  // namespace bd::app
