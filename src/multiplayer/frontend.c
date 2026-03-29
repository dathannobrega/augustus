#include "frontend.h"

#ifdef ENABLE_MULTIPLAYER

#include "mp_debug_log.h"

#include <string.h>

#if !defined(CLAUDIUS_DEDICATED_SERVER)
#include "window/city.h"
#include "window/main_menu.h"
#include "window/multiplayer_resume_lobby.h"
#endif

static struct {
    mp_frontend_callbacks callbacks;
    int initialized;
} frontend_data;

static void default_enter_game(void)
{
#if !defined(CLAUDIUS_DEDICATED_SERVER)
    window_city_show();
#else
    MP_LOG_INFO("FRONTEND", "Headless enter_game ignored");
#endif
}

static void default_show_resume_lobby(void)
{
#if !defined(CLAUDIUS_DEDICATED_SERVER)
    window_multiplayer_resume_lobby_show();
#else
    MP_LOG_INFO("FRONTEND", "Headless resume lobby ignored");
#endif
}

static void default_return_to_menu(int restart_music)
{
#if !defined(CLAUDIUS_DEDICATED_SERVER)
    window_main_menu_show(restart_music);
#else
    MP_LOG_INFO("FRONTEND", "Headless return_to_menu ignored (restart_music=%d)",
                restart_music);
#endif
}

static void ensure_initialized(void)
{
    if (frontend_data.initialized) {
        return;
    }

    memset(&frontend_data, 0, sizeof(frontend_data));
    frontend_data.callbacks.enter_game = default_enter_game;
    frontend_data.callbacks.show_resume_lobby = default_show_resume_lobby;
    frontend_data.callbacks.return_to_menu = default_return_to_menu;
    frontend_data.initialized = 1;
}

void mp_frontend_set_callbacks(const mp_frontend_callbacks *callbacks)
{
    ensure_initialized();
    if (!callbacks) {
        return;
    }
    frontend_data.callbacks = *callbacks;
}

void mp_frontend_enter_game(void)
{
    ensure_initialized();
    if (frontend_data.callbacks.enter_game) {
        frontend_data.callbacks.enter_game();
    }
}

void mp_frontend_show_resume_lobby(void)
{
    ensure_initialized();
    if (frontend_data.callbacks.show_resume_lobby) {
        frontend_data.callbacks.show_resume_lobby();
    }
}

void mp_frontend_return_to_menu(int restart_music)
{
    ensure_initialized();
    if (frontend_data.callbacks.return_to_menu) {
        frontend_data.callbacks.return_to_menu(restart_music);
    }
}

#endif /* ENABLE_MULTIPLAYER */
