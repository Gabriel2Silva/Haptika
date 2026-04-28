#pragma once
#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct _ConttestWindow ConttestWindow;

#define CONTTEST_TYPE_SELECTOR_VIEW (conttest_selector_view_get_type())
G_DECLARE_FINAL_TYPE(ConttestSelectorView, conttest_selector_view, CONTTEST, SELECTOR_VIEW, GtkBox)

GtkWidget *conttest_selector_view_new(ConttestWindow *window);

G_END_DECLS
