#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "app-state.h"
#include "log.h"
#include "widgets.h"
#include "window.h"

namespace {

// The SDL window's position and size, and the global font size, kept in the
// ImGui ini file as "[pgui][Window]": read from it before the window is
// created, and written from the live window and ImGui's FontSizeBase, when
// ImGui saves settings, once it is set. The window is created with its frame at
// x, y but reports where its contents are, so the offset between them, measured
// once it is shown, is taken off the position written.
struct WindowGeometry {
  int x = SDL_WINDOWPOS_CENTERED;
  int y = SDL_WINDOWPOS_CENTERED;
  int width = 1280;
  int height = 960;
  SDL_Window* window = nullptr;
  int frame_left = 0;
  int frame_top = 0;
  int font_size = DEFAULT_FONT_SIZE;
};

WindowGeometry& window_geometry() {
  static WindowGeometry geometry;
  return geometry;
}

// The columns' font sizes are kept in the ini file as "[pgui][Columns]", on a
// "FontSizes=" line listing them in column order, comma-separated.
void* settings_open(ImGuiContext*, ImGuiSettingsHandler*, char const* name) {
  if (std::strcmp(name, "Window") == 0) return &window_geometry();
  if (std::strcmp(name, "Columns") == 0) return &main_window();
  return nullptr;
}

void read_column_font_sizes(char const* line) {
  char const* const prefix = "FontSizes=";
  if (std::strncmp(line, prefix, std::strlen(prefix)) != 0) return;
  char const* p = line + std::strlen(prefix);
  Window& window = main_window();
  for (int i = 0; i < window.column_count(); ++i) {
    char* end;
    long const size = std::strtol(p, &end, 10);
    if (end == p) return;
    window.get_column(i).set_font_size(int(size));
    if (*end != ',') return;
    p = end + 1;
  }
}

void settings_read_line(ImGuiContext*, ImGuiSettingsHandler*, void* entry,
                        char const* line) {
  if (entry == &main_window()) {
    read_column_font_sizes(line);
    return;
  }
  auto* geometry = static_cast<WindowGeometry*>(entry);
  int a, b;
  if (std::sscanf(line, "Pos=%d,%d", &a, &b) == 2) {
    geometry->x = a;
    geometry->y = b;
  } else if (std::sscanf(line, "Size=%d,%d", &a, &b) == 2 && a > 0 && b > 0) {
    geometry->width = a;
    geometry->height = b;
  } else if (std::sscanf(line, "FontSize=%d", &a) == 1 &&
             a >= MIN_FONT_SIZE && a <= MAX_FONT_SIZE) {
    geometry->font_size = a;
  }
}

void settings_write_all(ImGuiContext*, ImGuiSettingsHandler*,
                        ImGuiTextBuffer* out) {
  Window& top = main_window();
  out->appendf("[pgui][Columns]\nFontSizes=");
  for (int i = 0; i < top.column_count(); ++i)
    out->appendf("%s%d", i ? "," : "", top.get_column(i).font_size());
  out->appendf("\n\n");
  SDL_Window* const window = window_geometry().window;
  if (!window) return;
  int x, y, width, height;
  SDL_GetWindowPosition(window, &x, &y);
  SDL_GetWindowSize(window, &width, &height);
  out->appendf("[pgui][Window]\nPos=%d,%d\nSize=%d,%d\nFontSize=%ld\n\n",
               x - window_geometry().frame_left,
               y - window_geometry().frame_top, width, height,
               std::lround(ImGui::GetStyle().FontSizeBase));
}

// Registers the "[pgui]" ini section and loads the ini file into
// window_geometry() and the columns' font sizes, ahead of the first
// NewFrame().
void load_settings() {
  ImGuiSettingsHandler handler;
  handler.TypeName = "pgui";
  handler.TypeHash = ImHashStr("pgui");
  handler.ReadOpenFn = settings_open;
  handler.ReadLineFn = settings_read_line;
  handler.WriteAllFn = settings_write_all;
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
      "/code/nutrimatic/idx/pairs.s1";

  if (!load_app_state(seed)) return 1;
  Window& top = main_window();
  ColumnIdentifier previous = top.add_column(Column(Seed{app_state().seed_key}, std::nullopt));
  for (int i = 1; i < 8; ++i)
    previous = top.add_column(Column(previous, previous));

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  load_settings();

  SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
    report("pgui", "SDL_Init: %s", SDL_GetError());
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
    report("pgui", "SDL window/renderer: %s", SDL_GetError());
    return 1;
  }
  SDL_RendererInfo info;
  if (SDL_GetRendererInfo(renderer, &info) == 0)
    report("pgui", "SDL renderer %s", info.name);

  ImGui::StyleColorsDark();
  ImGui::GetIO().Fonts->AddFontDefaultVector();
  load_letter_count_font();
  ImGui::GetStyle().FontSizeBase = float(window_geometry().font_size);
  ImGui::GetIO().ConfigInputTextCursorBlink = false;
  ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer2_Init(renderer);

  // Frames still to render before blocking for the next event; ImGui can
  // take a frame after an event to settle.
  int frames_due = 2;
  bool measured = false;
  bool interrupted = false;
  while (!interrupted && !top.closed()) {
    SDL_Event event;
    if (frames_due > 0 ? SDL_PollEvent(&event) : SDL_WaitEvent(&event)) {
      do {
        keypad_to_navigation(event);
        ImGui_ImplSDL2_ProcessEvent(&event);
        if (event.type == SDL_QUIT) interrupted = true;
        if (event.type == SDL_WINDOWEVENT &&
            event.window.event == SDL_WINDOWEVENT_CLOSE)
          top.request_close();
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

  if (top.running())
    report("pgui", "exiting: waiting on running tasks...");
  top.stop();

  ImGui_ImplSDLRenderer2_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  report("pgui", "exiting");
  return 0;
}
