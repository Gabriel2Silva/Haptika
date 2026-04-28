/*
 * test_sdl — list connected gamepads via SDL3.
 *
 * Useful for verifying SDL3 sees the same devices as the main application.
 */
#include <SDL3/SDL.h>
#include <stdio.h>

int main(void) {
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    int count = 0;
    SDL_JoystickID *gamepads = SDL_GetGamepads(&count);
    printf("Connected gamepads: %d\n", count);

    if (gamepads) {
        for (int i = 0; i < count; i++) {
            SDL_JoystickID id = gamepads[i];
            const char *name = SDL_GetGamepadNameForID(id);
            const char *path = SDL_GetGamepadPathForID(id);
            printf("  [%d] id=%d  name=\"%s\"  path=%s\n",
                   i, (int)id, name ? name : "(null)", path ? path : "(null)");
        }
        SDL_free(gamepads);
    }

    SDL_Quit();
    return 0;
}
