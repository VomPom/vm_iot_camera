//
// Created by vompom on on 2026/06/06 09:20.
//
// @Description
//

#ifndef VM_IOT_SIGNAL_HANDLER_H
#define VM_IOT_SIGNAL_HANDLER_H
#include <glib.h>
/* 把 SIGINT / SIGTERM 安全地转成 g_main_loop_quit
   用 g_unix_signal_add 而不是 signal()，避免在信号上下文里干 unsafe 调用 */
void install_signals(GMainLoop* loop);


#endif //VM_IOT_SIGNAL_HANDLER_H
