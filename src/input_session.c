#include "input_session.h"

void conttest_input_session_stop(ConttestInputSession *session) {
    if (!session) return;
    g_atomic_int_set(&session->atomic_stop_flag, 1);
    if (session->thread) {
        g_thread_join(session->thread);
    }
    if (session->free_func) {
        session->free_func(session);
    }
    g_free(session);
}

