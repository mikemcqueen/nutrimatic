#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "app-state.h"
#include "window.h"

namespace {

// The SDL window's position and size, kept in the ImGui ini file as
// "[pgui][Window]": read from it before the window is created, and written
// from the live window, when ImGui saves settings, once it is set. The window
// is created with its frame at x, y but reports where its contents are, so
// the offset between them, measured once it is shown, is taken off the
// position written.
struct WindowGeometry {
  int x = SDL_WINDOWPOS_CENTERED;
  int y = SDL_WINDOWPOS_CENTERED;
  int width = 1280;
  int height = 960;
  SDL_Window* window = nullptr;
  int frame_left = 0;
  int frame_top = 0;
};

WindowGeometry& window_geometry() {
  static WindowGeometry geometry;
  return geometry;
}

void* geometry_open(ImGuiContext*, ImGuiSettingsHandler*, char const* name) {
  return std::strcmp(name, "Window") == 0 ? &window_geometry() : nullptr;
}

void geometry_read_line(ImGuiContext*, ImGuiSettingsHandler*, void* entry,
                        char const* line) {
  auto* geometry = static_cast<WindowGeometry*>(entry);
  int a, b;
  if (std::sscanf(line, "Pos=%d,%d", &a, &b) == 2) {
    geometry->x = a;
    geometry->y = b;
  } else if (std::sscanf(line, "Size=%d,%d", &a, &b) == 2 && a > 0 && b > 0) {
    geometry->width = a;
    geometry->height = b;
  }
}

void geometry_write_all(ImGuiContext*, ImGuiSettingsHandler*,
                        ImGuiTextBuffer* out) {
  SDL_Window* const window = window_geometry().window;
  if (!window) return;
  int x, y, width, height;
  SDL_GetWindowPosition(window, &x, &y);
  SDL_GetWindowSize(window, &width, &height);
  out->appendf("[pgui][Window]\nPos=%d,%d\nSize=%d,%d\n\n",
               x - window_geometry().frame_left,
               y - window_geometry().frame_top, width, height);
}

// Registers the "[pgui]" ini section and loads the ini file into
// window_geometry(), ahead of the first NewFrame().
void load_window_geometry() {
  ImGuiSettingsHandler handler;
  handler.TypeName = "pgui";
  handler.TypeHash = ImHashStr("pgui");
  handler.ReadOpenFn = geometry_open;
  handler.ReadLineFn = geometry_read_line;
  handler.WriteAllFn = geometry_write_all;
  ImGui::AddSettingsHandler(&handler);
  ImGui::LoadIniSettingsFromDisk(ImGui::GetIO().IniFilename);
}

// Sets window_geometry()'s frame offset from where the window was put and
// where it reports being, when it was put at a position from the ini file.
void measure_window_frame() {
  WindowGeometry& geometry = window_geometry();
  if (SDL_WINDOWPOS_ISCENTERED(geometry.x)) return;
  int x, y;
  SDL_GetWindowPosition(geometry.window, &x, &y);
  geometry.frame_left = x - geometry.x;
  geometry.frame_top = y - geometry.y;
}

// Turns a keypad key pressed with NumLock off into the navigation key it
// stands for, as SDL leaves it a keypad key and ImGui navigation ignores
// keypad keys. Some keyboards (and WSLg) send PageUp/PageDown this way.
void keypad_to_navigation(SDL_Event& event) {
  if (event.type != SDL_KEYDOWN && event.type != SDL_KEYUP) return;
  SDL_Keysym& key = event.key.keysym;
  if (key.mod & KMOD_NUM) return;
  switch (key.sym) {
    case SDLK_KP_1: key.sym = SDLK_END; break;
    case SDLK_KP_2: key.sym = SDLK_DOWN; break;
    case SDLK_KP_3: key.sym = SDLK_PAGEDOWN; break;
    case SDLK_KP_4: key.sym = SDLK_LEFT; break;
    case SDLK_KP_6: key.sym = SDLK_RIGHT; break;
    case SDLK_KP_7: key.sym = SDLK_HOME; break;
    case SDLK_KP_8: key.sym = SDLK_UP; break;
    case SDLK_KP_9: key.sym = SDLK_PAGEUP; break;
    case SDLK_KP_0: key.sym = SDLK_INSERT; break;
    case SDLK_KP_PERIOD: key.sym = SDLK_DELETE; break;
    default: break;
  }
}

}  // namespace

int main(int, char**) {
  char const* home = std::getenv("HOME");
  std::string const seed = std::string(home ? home : "") +
      "/code/nutrimatic/idx/seed.s2.m3.all.85.15.pairs";

  if (!load_app_state(seed)) return 1;
  Window& top = main_window();
  ColumnIdentifier previous = top.add_column(Column(Seed{app_state().seed_key}, std::nullopt));
  for (int i = 1; i < 6; ++i)
    previous = top.add_column(Column(previous, previous));

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  load_window_geometry();

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  WindowGeometry& geometry = window_geometry();
  SDL_Window* window = SDL_CreateWindow(
      "pgui", geometry.x, geometry.y, geometry.width, geometry.height,
      SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  geometry.window = window;
  SDL_Renderer* renderer = SDL_CreateRenderer(
      window, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
  if (!window || !renderer) {
    std::fprintf(stderr, "SDL window/renderer: %s\n", SDL_GetError());
    return 1;
  }
  SDL_RendererInfo info;
  if (SDL_GetRendererInfo(renderer, &info) == 0)
    std::fprintf(stderr, "pgui: SDL renderer %s\n", info.name);

  ImGui::StyleColorsDark();
  ImGui::GetIO().Fonts->AddFontDefaultVector();
  ImGui::GetStyle().FontSizeBase = 20.0f;
  ImGui::GetIO().ConfigInputTextCursorBlink = false;
  ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer2_Init(renderer);

  // Frames still to render before blocking for the next event; ImGui can
  // take a frame after an event to settle.
  int frames_due = 2;
  bool running = true;
  bool measured = false;
  while (running) {
    SDL_Event event;
    if (frames_due > 0 ? SDL_PollEvent(&event) : SDL_WaitEvent(&event)) {
      do {
        keypad_to_navigation(event);
        ImGui_ImplSDL2_ProcessEvent(&event);
        if (event.type == SDL_QUIT) running = false;
        if (event.type == SDL_WINDOWEVENT &&
            (event.window.event == SDL_WINDOWEVENT_MOVED ||
             event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED))
          ImGui::MarkIniSettingsDirty();
        if (event.type == SDL_KEYDOWN &&
            event.key.keysym.sym == SDLK_F5)
          frames_due = 3;
      } while (SDL_PollEvent(&event));
      frames_due = std::max(frames_due, 2);
    }
    if (frames_due > 0) --frames_due;
    if (!measured) {
      measure_window_frame();
      measured = true;
    }

    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    top.render();

    ImGui::Render();
    SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
    SDL_RenderPresent(renderer);
  }

  ImGui_ImplSDLRenderer2_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
