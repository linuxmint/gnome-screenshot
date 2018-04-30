/* screenshot-area-selection.c - interactive screenshot area selection
 *
 * Copyright (C) 2001-2006  Jonathan Blandford <jrb@alum.mit.edu>
 * Copyright (C) 2008 Cosimo Cecchi <cosimoc@gnome.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 */

#include "config.h"

#include <gtk/gtk.h>

#include "screenshot-area-selection.h"

typedef struct {
  GdkRectangle  rect;
  gint          start_x;
  gint          start_y;
  gboolean      button_pressed;
  GtkWidget    *window;

  gboolean      aborted;
} select_area_filter_data;

static gboolean
select_area_button_press (GtkWidget               *window,
                          GdkEventButton          *event,
                          select_area_filter_data *data)
{
  if (data->button_pressed)
    return TRUE;

  data->button_pressed = TRUE;
  data->start_x = event->x_root;
  data->start_y = event->y_root;

  return TRUE;
}

static gboolean
select_area_motion_notify (GtkWidget               *window,
                           GdkEventMotion          *event,
                           select_area_filter_data *data)
{
  GdkRectangle draw_rect;

  if (!data->button_pressed)
    return TRUE;

  draw_rect.width = ABS (data->start_x - event->x_root);
  draw_rect.height = ABS (data->start_y - event->y_root);
  draw_rect.x = MIN (data->start_x, event->x_root);
  draw_rect.y = MIN (data->start_y, event->y_root);

  data->rect.x = draw_rect.x;
  data->rect.y = draw_rect.y;
  data->rect.width = draw_rect.width;
  data->rect.height = draw_rect.height;

  gtk_widget_queue_draw(window);

  return TRUE;
}

static gboolean
select_area_button_release (GtkWidget               *window,
                            GdkEventButton          *event,
                            select_area_filter_data *data)
{
  GdkRectangle draw_rect;
  if (!data->button_pressed)
    return TRUE;

  draw_rect.width = ABS (data->start_x - event->x_root);
  draw_rect.height = ABS (data->start_y - event->y_root);
  draw_rect.x = MIN (data->start_x, event->x_root);
  draw_rect.y = MIN (data->start_y, event->y_root);

  data->rect.x = draw_rect.x;
  data->rect.y = draw_rect.y;
  data->rect.width = draw_rect.width;
  data->rect.height = draw_rect.height;

  if (data->rect.width == 0 || data->rect.height == 0)
    data->aborted = TRUE;

  gtk_main_quit ();

  return TRUE;
}

static gboolean
select_area_key_press (GtkWidget               *window,
                       GdkEventKey             *event,
                       select_area_filter_data *data)
{
  if (event->keyval == GDK_KEY_Escape)
    {
      data->start_x = 0;
      data->start_y = 0;
      data->rect.x = 0;
      data->rect.y = 0;
      data->rect.width  = 0;
      data->rect.height = 0;
      data->aborted = TRUE;

      gtk_main_quit ();
    }
 
  return TRUE;
}

static gboolean
select_window_draw (GtkWidget *window, cairo_t *cr, gpointer user_data)
{
  GtkStyleContext *style;
  select_area_filter_data *data = user_data;

  style = gtk_widget_get_style_context (window);

  if (gtk_widget_get_app_paintable (window))
    {
      cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
      cairo_set_source_rgba (cr, 0, 0, 0, 0);
      cairo_paint (cr);

      gtk_style_context_save (style);
      gtk_style_context_add_class (style, GTK_STYLE_CLASS_RUBBERBAND);

      gtk_render_background (style, cr,
                             0, 0,
                             gtk_widget_get_allocated_width (window),
                             gtk_widget_get_allocated_height (window));
      gtk_render_frame (style, cr,
                        0, 0,
                        gtk_widget_get_allocated_width (window),
                        gtk_widget_get_allocated_height (window));

      cairo_rectangle(cr, data->rect.x, data->rect.y, data->rect.width,
                      data->rect.height);
      cairo_fill(cr);
      gtk_render_frame(style, cr, 0, 0, gtk_widget_get_allocated_width(window),
                      gtk_widget_get_allocated_height(window));

      gtk_style_context_restore(style);
    }

  return TRUE;
}

static GtkWidget *
create_select_window (void)
{
  GtkWidget *window;
  GdkScreen *screen;
  GdkVisual *visual;

  screen = gdk_screen_get_default ();
  visual = gdk_screen_get_rgba_visual (screen);

  window = gtk_window_new (GTK_WINDOW_POPUP);
  if (gdk_screen_is_composited (screen) && visual)
    {
      gtk_widget_set_visual (window, visual);
      gtk_widget_set_app_paintable (window, TRUE);
    }

  gtk_window_move(GTK_WINDOW(window), 0, 0);
  gtk_window_resize(GTK_WINDOW(window),
                    gdk_screen_get_width(screen),
                    gdk_screen_get_height(screen));
  gtk_widget_show(window);

  return window;
}

typedef struct {
  GdkRectangle rectangle;
  SelectAreaCallback callback;
  gpointer callback_data;
  gboolean aborted;
} CallbackData;

static gboolean
emit_select_callback_in_idle (gpointer user_data)
{
  CallbackData *data = user_data;

  if (!data->aborted)
    data->callback (&data->rectangle, data->callback_data);
  else
    data->callback (NULL, data->callback_data);

  g_slice_free (CallbackData, data);

  return FALSE;
}

static void
screenshot_select_area_x11_async (CallbackData *cb_data)
{
  GdkCursor *cursor;
  select_area_filter_data  data;
  GdkDeviceManager *manager;
  GdkDevice *pointer, *keyboard;
  GdkGrabStatus res;

  data.rect.x = 0;
  data.rect.y = 0;
  data.rect.width  = 0;
  data.rect.height = 0;
  data.button_pressed = FALSE;
  data.aborted = FALSE;
  data.window = create_select_window();

  g_signal_connect(data.window, "draw", G_CALLBACK(select_window_draw), &data);
  g_signal_connect (data.window, "key-press-event", G_CALLBACK (select_area_key_press), &data);
  g_signal_connect (data.window, "button-press-event", G_CALLBACK (select_area_button_press), &data);
  g_signal_connect (data.window, "button-release-event", G_CALLBACK (select_area_button_release), &data);
  g_signal_connect (data.window, "motion-notify-event", G_CALLBACK (select_area_motion_notify), &data);

  cursor = gdk_cursor_new (GDK_CROSSHAIR);
  manager = gdk_display_get_device_manager (gdk_display_get_default ());
  pointer = gdk_device_manager_get_client_pointer (manager);
  keyboard = gdk_device_get_associated_device (pointer);

  res = gdk_device_grab (pointer, gtk_widget_get_window (data.window),
                         GDK_OWNERSHIP_NONE, FALSE,
                         GDK_POINTER_MOTION_MASK |
                         GDK_BUTTON_PRESS_MASK | 
                         GDK_BUTTON_RELEASE_MASK,
                         cursor, GDK_CURRENT_TIME);

  if (res != GDK_GRAB_SUCCESS)
    {
      g_object_unref (cursor);
      goto out;
    }

  res = gdk_device_grab (keyboard, gtk_widget_get_window (data.window),
                         GDK_OWNERSHIP_NONE, FALSE,
                         GDK_KEY_PRESS_MASK |
                         GDK_KEY_RELEASE_MASK,
                         NULL, GDK_CURRENT_TIME);
  if (res != GDK_GRAB_SUCCESS)
    {
      gdk_device_ungrab (pointer, GDK_CURRENT_TIME);
      g_object_unref (cursor);
      goto out;
    }

  gtk_main ();

  gdk_device_ungrab (pointer, GDK_CURRENT_TIME);
  gdk_device_ungrab (keyboard, GDK_CURRENT_TIME);

  gtk_widget_destroy (data.window);
  g_object_unref (cursor);

  gdk_flush ();

 out:
  cb_data->aborted = data.aborted;
  cb_data->rectangle = data.rect;

  emit_select_callback_in_idle(cb_data);
}

static void
select_area_done (GObject *source_object,
                  GAsyncResult *res,
                  gpointer user_data)
{
  CallbackData *cb_data = user_data;
  GError *error = NULL;
  GVariant *ret;

  ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source_object), res, &error);
  if (error != NULL)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_error_free (error);
          cb_data->aborted = TRUE;
          g_idle_add (emit_select_callback_in_idle, cb_data);
          return;
        }

      g_message ("Unable to select area using GNOME Shell's builtin screenshot "
                 "interface, resorting to fallback X11.");
      g_error_free (error);

      screenshot_select_area_x11_async (cb_data);
      return;
    }

  g_variant_get (ret, "(iiii)",
                 &cb_data->rectangle.x,
                 &cb_data->rectangle.y,
                 &cb_data->rectangle.width,
                 &cb_data->rectangle.height);
  g_variant_unref (ret);

  g_idle_add (emit_select_callback_in_idle, cb_data);
}

void
screenshot_select_area_async (SelectAreaCallback callback,
                              gpointer callback_data)
{
  CallbackData *cb_data;
  GDBusConnection *connection;

  cb_data = g_slice_new0 (CallbackData);
  cb_data->callback = callback;
  cb_data->callback_data = callback_data;

  connection = g_application_get_dbus_connection (g_application_get_default ());
  g_dbus_connection_call (connection,
                          "org.gnome.Shell.Screenshot",
                          "/org/gnome/Shell/Screenshot",
                          "org.gnome.Shell.Screenshot",
                          "SelectArea",
                          NULL,
                          NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          G_MAXINT,
                          NULL,
                          select_area_done,
                          cb_data);
}
