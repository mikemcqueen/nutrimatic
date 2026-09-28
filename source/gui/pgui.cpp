#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "app-state.h"
#include "window.h"

int main(int, char**) {
  char const* home = std::getenv("HOME");
  std::string const seed = std::string(home ? home : "") +
      "/code/nutrimatic/idx/seed.s9.m4.all.85.15.pairs";

  if (!load_app_state()) return 1;
  Window& top = main_window();
  ColumnIdentifier const first =
      top.add_column(Column(seed, {"", "pfilter", "freqsort"}));
  top.add_column(Column(first, {"", "pfilter", "freqsort"}));

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  SDL_Window* window = SDL_CreateWindow(
      "pgui", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 960,
      SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  SDL_Renderer* renderer = SDL_CreateRenderer(
      window, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
  if (!window || !renderer) {
    std::fprintf(stderr, "SDL window/renderer: %s\n", SDL_GetError());
    return 1;
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();
  ImGui::GetIO().Fonts->AddFontDefaultVector();
  ImGui::GetStyle().FontSizeBase = 20.0f;
  ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer2_Init(renderer);

  bool running = true;
  while (running) {
    SDL_Event event;
    if (SDL_WaitEventTimeout(&event, 100)) {
      do {
        ImGui_ImplSDL2_ProcessEvent(&event);
        if (event.type == SDL_QUIT) running = false;
      } while (SDL_PollEvent(&event));
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
