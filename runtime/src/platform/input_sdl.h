#pragma once
#include <SDL3/SDL.h>
namespace input {
// Main-thread calls from the SDL window/event loop.
void handle_event(const SDL_Event& event);
void update();
void set_prompt_window(SDL_Window* window);
// While the game asks for text the window title shows the prompt (the frame-rate title waits).
bool text_prompt_active();
}
