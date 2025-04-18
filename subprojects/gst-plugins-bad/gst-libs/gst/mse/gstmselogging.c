/* GStreamer
 *
 * SPDX-License-Identifier: LGPL-2.1
 *
 * Copyright (C) 2022, 2023 Collabora Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstmselogging-private.h"

GST_DEBUG_CATEGORY (gst_mse_debug);

void
gst_mse_init_logging (void)
{
  GST_DEBUG_CATEGORY_INIT (gst_mse_debug, "gst-mse", 0, "GstMse");
}

const gchar *
gst_mse_enum_value_nick (GType enum_type, gint mse_enum_value)
{
  GEnumClass *enum_class = (GEnumClass *) g_type_class_ref (enum_type);
  GEnumValue *enum_value = g_enum_get_value (enum_class, mse_enum_value);
  const gchar *nick = enum_value->value_nick;
  g_type_class_unref (enum_class);
  return nick;
}
