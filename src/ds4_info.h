#pragma once
#include <adwaita.h>
#include "device_manager.h"

G_BEGIN_DECLS

/* Show the DS4 info dialog, parented to `parent`. Fetches data asynchronously. */
void ds4_info_show(GtkWidget *parent, const ConttestDevice *device);

G_END_DECLS
