/* GStreamer
 * Copyright (C) 2019 Naveen Cherukuri <naveenc@xilinx.com>
 *                    Saurabh Sengar <saurabh.singh@xilinx.com>
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

#ifndef __GST_XLNX_VIDEO_SCALE_H__
#define __GST_XLNX_VIDEO_SCALE_H__

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideofilter.h>

G_BEGIN_DECLS

#define GST_TYPE_XLNX_VIDEO_SCALE (gst_xlnx_video_scale_get_type())
#define GST_XLNX_VIDEO_SCALE(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_XLNX_VIDEO_SCALE, GstXlnxVideoScale))
#define GST_XLNX_VIDEO_SCALE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_XLNX_VIDEO_SCALE, GstXlnxVideoScaleClass))
#define GST_XLNX_VIDEO_SCALE_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), GST_TYPE_XLNX_VIDEO_SCALE, GstXlnxVideoScale))
#define GST_IS_XLNX_VIDEO_SCALE(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_XLNX_VIDEO_SCALE))
#define GST_IS_XLNX_VIDEO_SCALE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_XLNX_VIDEO_SCALE))
#define GST_XLNX_VIDEO_SCALE_CAST(obj)       ((GstXlnxVideoScale *)(obj))

typedef struct _GstXlnxVideoScale GstXlnxVideoScale;
typedef struct _GstXlnxVideoScaleClass GstXlnxVideoScaleClass;

/**
 * GstXlnxVideoScale:
 *
 * Opaque data structure
 */
struct _GstXlnxVideoScale {
  GstVideoFilter element;
  gint fbrd_fd;
  gint fbwr_fd;
  gint vpss_fd;
};

struct _GstXlnxVideoScaleClass {
  GstVideoFilterClass parent_class;
  GMutex time_division_lock;
  GstVideoInfo in_vinfo;
  GstVideoInfo out_vinfo;
};

G_GNUC_INTERNAL GType gst_xlnx_video_scale_get_type (void);

G_END_DECLS

#endif /* __GST_XLNX_VIDEO_SCALE_H__ */
