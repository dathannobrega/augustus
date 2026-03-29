#ifndef MULTIPLAYER_FRONTEND_H
#define MULTIPLAYER_FRONTEND_H

#ifdef ENABLE_MULTIPLAYER

typedef struct {
    void (*enter_game)(void);
    void (*show_resume_lobby)(void);
    void (*return_to_menu)(int restart_music);
} mp_frontend_callbacks;

void mp_frontend_set_callbacks(const mp_frontend_callbacks *callbacks);
void mp_frontend_enter_game(void);
void mp_frontend_show_resume_lobby(void);
void mp_frontend_return_to_menu(int restart_music);

#endif /* ENABLE_MULTIPLAYER */

#endif /* MULTIPLAYER_FRONTEND_H */
