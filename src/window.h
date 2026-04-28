#pragma once
#include <adwaita.h>

#include "input_session.h"

G_BEGIN_DECLS

typedef struct _ConttestDevice ConttestDevice;
typedef struct _ConttestDeviceManager ConttestDeviceManager;

#define CONTTEST_TYPE_WINDOW (conttest_window_get_type())
G_DECLARE_FINAL_TYPE(ConttestWindow, conttest_window, CONTTEST, WINDOW, AdwApplicationWindow)

GtkWidget *conttest_window_new(GtkApplication *app);

void conttest_window_show_selector(ConttestWindow *self);
void conttest_window_show_input_viewer(ConttestWindow *self, ConttestDevice *device, ConttestBackend backend);
void conttest_window_show_toast(ConttestWindow *self, const char *message);

ConttestDeviceManager *conttest_window_get_device_manager(ConttestWindow *self);

G_END_DECLS
