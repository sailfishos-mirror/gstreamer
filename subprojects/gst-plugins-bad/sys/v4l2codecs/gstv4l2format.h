/* GStreamer
 * Copyright (C) 2020 Nicolas Dufresne <nicolas.dufresne@collabora.com>
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

#pragma once

#include <gst/gst.h>
#include <gst/video/video.h>
#include "linux/videodev2.h"

/*
 * Ordered similar to what libgstvideo does, but keeping tiled formats first,
 * and prefering bandwidth over alignment (NV12_10LE40 over P010_LE).
 */
#define GST_V4L2_DEFAULT_VIDEO_FORMATS "{ " \
    "NV16_10LE40, " \
    "NV16, " \
    "MT2110R, " \
    "MT2110T, " \
    "NV12_10LE40_4L4, " \
    "NV12_10LE40, " \
    "P010_10LE, " \
    "YUY2, " \
    "NV12_16L32S, " \
    "NV12_32L32, " \
    "NV12_4L4, " \
    "NV12, " \
    "I420, " \
    "}"


gboolean   gst_v4l2_format_to_dma_drm_info (struct v4l2_format * fmt,
                                            GstVideoInfoDmaDrm * out_drm_info);

gboolean   gst_v4l2_format_to_video_format (guint32 pix_fmt,
                                            GstVideoFormat * out_format);

gboolean   gst_v4l2_format_to_drm_format (guint32 pix_fmt,
                                          guint32 * out_drm_fourcc,
                                          guint64 * out_drm_mod);

gboolean   gst_v4l2_format_from_video_format (GstVideoFormat format,
                                              guint32 * out_pix_fmt);

gboolean   gst_v4l2_format_from_drm_format (guint32 drm_fourcc,
                                            guint64 drm_mod,
                                            guint32 * out_pix_fmt);

guint      gst_v4l2_format_get_n_planes (GstVideoInfoDmaDrm * info);

GstCaps *  gst_v4l2_format_sort_caps (GstCaps * caps);
