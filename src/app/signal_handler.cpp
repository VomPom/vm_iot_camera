//
// Created by vompom on on 2026/06/06 09:20.
//
// @Description
//

#include "signal_handler.h"
#include "signal_handler.h"
#include "log.h"
#include <glib-unix.h>


static gboolean on_term(gpointer user) {
    auto *loop = static_cast<GMainLoop *>(user);
    LOGI("signal received, quitting main loop");
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

void install_signals(GMainLoop *loop) {
    g_unix_signal_add(SIGINT, on_term, loop);
    g_unix_signal_add(SIGTERM, on_term, loop);
}
