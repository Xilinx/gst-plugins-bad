/* GStreamer
 *
 * Copyright (C) 2016 Igalia
 *
 * Authors:
 *  Víctor Manuel Jáquez Leal <vjaquez@igalia.com>
 *  Javier Martin <javiermartin@by.com.es>
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
 *
 */

/**
 * SECTION:element-kmssink
 * @title: kmssink
 * @short_description: A KMS/DRM based video sink
 *
 * kmssink is a simple video sink that renders video frames directly
 * in a plane of a DRM device.
 *
 * In advance usage, the behaviour of kmssink can be change using the
 * supported properties. Note that plane and connectors IDs and properties can
 * be enumerated using the modetest command line tool.
 *
 * ## Example launch line
 * |[
 * gst-launch-1.0 videotestsrc ! kmssink
 * gst-launch-1.0 videotestsrc ! kmssink plane-properties=s,rotation=4
 * ]|
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/video/video.h>
#include <gst/video/videooverlay.h>
#include <gst/allocators/gstdmabuf.h>

#include <drm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <string.h>

#include "gstkmssink.h"
#include "gstkmsutils.h"
#include "gstkmsbufferpool.h"
#include "gstkmsallocator.h"

#define GST_PLUGIN_NAME "kmssink"
#define GST_PLUGIN_DESC "Video sink using the Linux kernel mode setting API"
#define OMX_ALG_GST_EVENT_INSERT_PREFIX_SEI "omx-alg/sei-parsed"
#define VSYNC_GAP_USEC 2500

#ifndef DRM_MODE_FB_ALTERNATE_TOP
#  define DRM_MODE_FB_ALTERNATE_TOP       (1<<2)        /* for alternate top field */
#endif
#ifndef DRM_MODE_FB_ALTERNATE_BOTTOM
#  define DRM_MODE_FB_ALTERNATE_BOTTOM    (1<<3)        /* for alternate bottom field */
#endif

#define LUMA_PLANE                        0
#define CHROMA_PLANE                      1
#define ROI_RECT_THICKNESS_MIN            0
#define ROI_RECT_THICKNESS_MAX            5
#define ROI_RECT_COLOR_MIN                0
#define ROI_RECT_COLOR_MAX                255

#define GRAY_HEIGHT_MAX                   6480
#ifndef DRM_FORMAT_Y8
#define DRM_FORMAT_Y8		fourcc_code('G', 'R', 'E', 'Y') /* 8  Greyscale */
#endif
#ifndef DRM_FORMAT_Y10
#define DRM_FORMAT_Y10		fourcc_code('Y', '1', '0', ' ') /* 10 Greyscale */
#endif
#ifndef DRM_FORMAT_X403
#define DRM_FORMAT_X403		fourcc_code('X', '4', '0', '3') /* non-subsampled Cb:Cr plane 2:10:10:10 */
#endif


GST_DEBUG_CATEGORY_STATIC (gst_kms_sink_debug);
GST_DEBUG_CATEGORY_STATIC (CAT_PERFORMANCE);
#define GST_CAT_DEFAULT gst_kms_sink_debug

/* sink is zynqmp DisplayPort */
gboolean is_dp = FALSE;

static GstFlowReturn gst_kms_sink_show_frame (GstVideoSink * vsink,
    GstBuffer * buf);
static void gst_kms_sink_video_overlay_init (GstVideoOverlayInterface * iface);
static void gst_kms_sink_drain (GstKMSSink * self);
static void ensure_kms_allocator (GstKMSSink * self);
static gboolean gst_kms_sink_sink_event (GstBaseSink * bsink, GstEvent * event);

#define parent_class gst_kms_sink_parent_class
G_DEFINE_TYPE_WITH_CODE (GstKMSSink, gst_kms_sink, GST_TYPE_VIDEO_SINK,
    GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, GST_PLUGIN_NAME, 0,
        GST_PLUGIN_DESC);
    GST_DEBUG_CATEGORY_GET (CAT_PERFORMANCE, "GST_PERFORMANCE");
    G_IMPLEMENT_INTERFACE (GST_TYPE_VIDEO_OVERLAY,
        gst_kms_sink_video_overlay_init));

enum
{
  PROP_DRIVER_NAME = 1,
  PROP_BUS_ID,
  PROP_CONNECTOR_ID,
  PROP_PLANE_ID,
  PROP_FORCE_MODESETTING,
  PROP_RESTORE_CRTC,
  PROP_CAN_SCALE,
  PROP_DISPLAY_WIDTH,
  PROP_DISPLAY_HEIGHT,
  PROP_HOLD_EXTRA_SAMPLE,
  PROP_DO_TIMESTAMP,
  PROP_AVOID_FIELD_INVERSION,
  PROP_CONNECTOR_PROPS,
  PROP_PLANE_PROPS,
  PROP_FULLSCREEN_OVERLAY,
  PROP_FORCE_NTSC_TV,
  PROP_GRAY_TO_YUV444,
  PROP_DRAW_ROI,
  PROP_ROI_RECT_THICKNESS,
  PROP_ROI_RECT_COLOR,
  PROP_N,
};

enum
{
  DRM_STATIC_METADATA_TYPE1 = 1,
};

enum
{
  DRM_EOTF_TRADITIONAL_GAMMA_SDR,
  DRM_EOTF_TRADITIONAL_GAMMA_HDR,
  DRM_EOTF_SMPTE_ST2084,
  DRM_EOTF_BT_2100_HLG,
};

static GParamSpec *g_properties[PROP_N] = { NULL, };

static void
gst_kms_sink_set_render_rectangle (GstVideoOverlay * overlay,
    gint x, gint y, gint width, gint height)
{
  GstKMSSink *self = GST_KMS_SINK (overlay);

  GST_DEBUG_OBJECT (self, "Setting render rectangle to (%d,%d) %dx%d", x, y,
      width, height);

  GST_OBJECT_LOCK (self);

  if (width == -1 && height == -1) {
    x = 0;
    y = 0;
    width = self->hdisplay;
    height = self->vdisplay;
  }

  if (width <= 0 || height <= 0)
    goto done;

  self->pending_rect.x = x;
  self->pending_rect.y = y;
  self->pending_rect.w = width;
  self->pending_rect.h = height;

  if (self->can_scale ||
      (self->render_rect.w == width && self->render_rect.h == height)) {
    self->render_rect = self->pending_rect;
  } else {
    self->reconfigure = TRUE;
    GST_DEBUG_OBJECT (self, "Waiting for new caps to apply render rectangle");
  }

done:
  GST_OBJECT_UNLOCK (self);
}

static void
gst_kms_sink_expose (GstVideoOverlay * overlay)
{
  GstKMSSink *self = GST_KMS_SINK (overlay);

  GST_DEBUG_OBJECT (overlay, "Expose called by application");

  if (!self->can_scale) {
    GST_OBJECT_LOCK (self);
    if (self->reconfigure) {
      GST_OBJECT_UNLOCK (self);
      GST_DEBUG_OBJECT (overlay, "Sending a reconfigure event");
      gst_pad_push_event (GST_BASE_SINK_PAD (self),
          gst_event_new_reconfigure ());
    } else {
      GST_DEBUG_OBJECT (overlay, "Applying new render rectangle");
      /* size of the rectangle does not change, only the (x,y) position changes */
      self->render_rect = self->pending_rect;
      GST_OBJECT_UNLOCK (self);
    }
  }

  gst_kms_sink_show_frame (GST_VIDEO_SINK (self), NULL);
}

static void
gst_kms_sink_video_overlay_init (GstVideoOverlayInterface * iface)
{
  iface->expose = gst_kms_sink_expose;
  iface->set_render_rectangle = gst_kms_sink_set_render_rectangle;
}

static int
kms_open (gchar ** driver)
{
  static const char *drivers[] = { "i915", "radeon", "nouveau", "vmwgfx",
    "exynos", "amdgpu", "imx-drm", "rockchip", "atmel-hlcdc", "msm",
    "xlnx", "vc4", "meson", "sun4i-drm", "mxsfb-drm",
    "xilinx_drm",               /* DEPRECATED. Replaced by xlnx */
  };
  int i, fd = -1;

  for (i = 0; i < G_N_ELEMENTS (drivers); i++) {
    fd = drmOpen (drivers[i], NULL);
    if (fd >= 0) {
      if (driver)
        *driver = g_strdup (drivers[i]);
      break;
    }
  }

  return fd;
}

static guint64
find_property_value_for_plane_id (gint fd, gint plane_id, const char *prop_name)
{
  drmModeObjectPropertiesPtr properties;
  drmModePropertyPtr property;
  gint i, prop_value;

  properties = drmModeObjectGetProperties (fd, plane_id, DRM_MODE_OBJECT_PLANE);

  for (i = 0; i < properties->count_props; i++) {
    property = drmModeGetProperty (fd, properties->props[i]);
    if (!strcmp (property->name, prop_name)) {
      prop_value = properties->prop_values[i];
      drmModeFreeProperty (property);
      drmModeFreeObjectProperties (properties);
      return prop_value;
    }
  }

  return -1;

}

static gboolean
set_property_value_for_plane_id (gint fd, gint plane_id, const char *prop_name,
    gint value)
{
  drmModeObjectPropertiesPtr properties;
  drmModePropertyPtr property;
  gint i;
  gboolean ret = FALSE;

  properties = drmModeObjectGetProperties (fd, plane_id, DRM_MODE_OBJECT_PLANE);

  for (i = 0; i < properties->count_props && !ret; i++) {
    property = drmModeGetProperty (fd, properties->props[i]);
    if (!strcmp (property->name, prop_name)) {
      drmModeObjectSetProperty (fd, plane_id,
          DRM_MODE_OBJECT_PLANE, property->prop_id, value);
      ret = TRUE;
    }
    drmModeFreeProperty (property);
  }

  drmModeFreeObjectProperties (properties);
  return ret;
}

static drmModePlane *
find_plane_for_crtc (int fd, drmModeRes * res, drmModePlaneRes * pres,
    int crtc_id, int plane_type)
{
  drmModePlane *plane;
  int i, pipe, value;

  plane = NULL;
  pipe = -1;
  for (i = 0; i < res->count_crtcs; i++) {
    if (crtc_id == res->crtcs[i]) {
      pipe = i;
      break;
    }
  }

  if (pipe == -1)
    return NULL;

  for (i = 0; i < pres->count_planes; i++) {
    plane = drmModeGetPlane (fd, pres->planes[i]);
    if (plane_type != -1) {
      value = find_property_value_for_plane_id (fd, pres->planes[i], "type");
      if (plane_type != value)
        continue;
    }
    if (plane->possible_crtcs & (1 << pipe))
      return plane;
    drmModeFreePlane (plane);
  }

  return NULL;
}

static drmModeCrtc *
find_crtc_for_connector (int fd, drmModeRes * res, drmModeConnector * conn,
    guint * pipe)
{
  int i;
  int crtc_id;
  drmModeEncoder *enc;
  drmModeCrtc *crtc;
  guint32 crtcs_for_connector = 0;

  crtc_id = -1;
  for (i = 0; i < res->count_encoders; i++) {
    enc = drmModeGetEncoder (fd, res->encoders[i]);
    if (enc) {
      if (enc->encoder_id == conn->encoder_id) {
        crtc_id = enc->crtc_id;
        drmModeFreeEncoder (enc);
        break;
      }
      drmModeFreeEncoder (enc);
    }
  }

  /* If no active crtc was found, pick the first possible crtc */
  if (crtc_id == -1) {
    for (i = 0; i < conn->count_encoders; i++) {
      enc = drmModeGetEncoder (fd, conn->encoders[i]);
      crtcs_for_connector |= enc->possible_crtcs;
      drmModeFreeEncoder (enc);
    }

    if (crtcs_for_connector != 0)
      crtc_id = res->crtcs[ffs (crtcs_for_connector) - 1];
  }

  if (crtc_id == -1)
    return NULL;

  for (i = 0; i < res->count_crtcs; i++) {
    crtc = drmModeGetCrtc (fd, res->crtcs[i]);
    if (crtc) {
      if (crtc_id == crtc->crtc_id) {
        if (pipe)
          *pipe = i;
        return crtc;
      }
      drmModeFreeCrtc (crtc);
    }
  }

  return NULL;
}

static gboolean
connector_is_used (int fd, drmModeRes * res, drmModeConnector * conn)
{
  gboolean result;
  drmModeCrtc *crtc;

  result = FALSE;
  crtc = find_crtc_for_connector (fd, res, conn, NULL);
  if (crtc) {
    result = crtc->buffer_id != 0;
    drmModeFreeCrtc (crtc);
  }

  return result;
}

static drmModeConnector *
find_used_connector_by_type (int fd, drmModeRes * res, int type)
{
  int i;
  drmModeConnector *conn;

  conn = NULL;
  for (i = 0; i < res->count_connectors; i++) {
    conn = drmModeGetConnector (fd, res->connectors[i]);
    if (conn) {
      if ((conn->connector_type == type) && connector_is_used (fd, res, conn))
        return conn;
      drmModeFreeConnector (conn);
    }
  }

  return NULL;
}

static drmModeConnector *
find_first_used_connector (int fd, drmModeRes * res)
{
  int i;
  drmModeConnector *conn;

  conn = NULL;
  for (i = 0; i < res->count_connectors; i++) {
    conn = drmModeGetConnector (fd, res->connectors[i]);
    if (conn) {
      if (connector_is_used (fd, res, conn))
        return conn;
      drmModeFreeConnector (conn);
    }
  }

  return NULL;
}

static drmModeConnector *
find_main_monitor (int fd, drmModeRes * res)
{
  /* Find the LVDS and eDP connectors: those are the main screens. */
  static const int priority[] = { DRM_MODE_CONNECTOR_LVDS,
    DRM_MODE_CONNECTOR_eDP
  };
  int i;
  drmModeConnector *conn;

  conn = NULL;
  for (i = 0; !conn && i < G_N_ELEMENTS (priority); i++)
    conn = find_used_connector_by_type (fd, res, priority[i]);

  /* if we didn't find a connector, grab the first one in use */
  if (!conn)
    conn = find_first_used_connector (fd, res);

  /* if no connector is used, grab the first one */
  if (!conn)
    conn = drmModeGetConnector (fd, res->connectors[0]);

  return conn;
}

static void
log_drm_version (GstKMSSink * self)
{
#ifndef GST_DISABLE_GST_DEBUG
  drmVersion *v;

  v = drmGetVersion (self->fd);
  if (v) {
    GST_INFO_OBJECT (self, "DRM v%d.%d.%d [%s — %s — %s]", v->version_major,
        v->version_minor, v->version_patchlevel, GST_STR_NULL (v->name),
        GST_STR_NULL (v->desc), GST_STR_NULL (v->date));
    drmFreeVersion (v);
  } else {
    GST_WARNING_OBJECT (self, "could not get driver information: %s",
        GST_STR_NULL (self->devname));
  }
#endif
  return;
}

static gboolean
get_drm_caps (GstKMSSink * self)
{
  gint ret;
  guint64 has_dumb_buffer;
  guint64 has_prime;
  guint64 has_async_page_flip;

  has_dumb_buffer = 0;
  ret = drmGetCap (self->fd, DRM_CAP_DUMB_BUFFER, &has_dumb_buffer);
  if (ret)
    GST_WARNING_OBJECT (self, "could not get dumb buffer capability");
  if (has_dumb_buffer == 0) {
    GST_ERROR_OBJECT (self, "driver cannot handle dumb buffers");
    return FALSE;
  }

  has_prime = 0;
  ret = drmGetCap (self->fd, DRM_CAP_PRIME, &has_prime);
  if (ret)
    GST_WARNING_OBJECT (self, "could not get prime capability");
  else {
    self->has_prime_import = (gboolean) (has_prime & DRM_PRIME_CAP_IMPORT);
    self->has_prime_export = (gboolean) (has_prime & DRM_PRIME_CAP_EXPORT);
  }

  has_async_page_flip = 0;
  ret = drmGetCap (self->fd, DRM_CAP_ASYNC_PAGE_FLIP, &has_async_page_flip);
  if (ret)
    GST_WARNING_OBJECT (self, "could not get async page flip capability");
  else
    self->has_async_page_flip = (gboolean) has_async_page_flip;

  GST_INFO_OBJECT (self,
      "prime import (%s) / prime export (%s) / async page flip (%s)",
      self->has_prime_import ? "✓" : "✗",
      self->has_prime_export ? "✓" : "✗",
      self->has_async_page_flip ? "✓" : "✗");

  return TRUE;
}

static void
ensure_kms_allocator (GstKMSSink * self)
{
  if (self->allocator)
    return;
  self->allocator = gst_kms_allocator_new (self->fd);
}

static gboolean
configure_mode_setting (GstKMSSink * self, GstVideoInfo * vinfo)
{
  gboolean ret;
  drmModeConnector *conn;
  int err;
  gint i;
  drmModeModeInfo *mode = NULL;
  drmModeModeInfo *cached_mode = NULL;
  guint32 fb_id;
  GstMemory *mem = 0;
  gfloat fps;
  gfloat vrefresh;
  ret = FALSE;
  conn = NULL;
  mode = NULL;

  if (self->vinfo_crtc.finfo
      && gst_video_info_is_equal (&self->vinfo_crtc, vinfo))
    return TRUE;

  if (self->conn_id < 0)
    goto bail;

  GST_INFO_OBJECT (self, "configuring mode setting");

  ensure_kms_allocator (self);
  mem = gst_kms_allocator_bo_alloc (self->allocator, vinfo);
  if (!mem)
    goto bo_failed;

  if (!gst_kms_memory_add_fb (mem, vinfo, 0))
    goto bo_failed;

  fb_id = gst_kms_memory_get_fb_id (mem);

  conn = drmModeGetConnector (self->fd, self->conn_id);
  if (!conn)
    goto connector_failed;

  fps = (gfloat) GST_VIDEO_INFO_FPS_N (vinfo) / GST_VIDEO_INFO_FPS_D (vinfo);

  if (self->force_ntsc_tv && vinfo->height == 480) {
    vinfo->height = 486;
    vinfo->width = 720;
    GST_LOG_OBJECT (self, "Forcing mode setting to NTSC TV D1(720x486i)");
  }

  for (i = 0; i < conn->count_modes; i++) {
    if (conn->modes[i].vdisplay == GST_VIDEO_INFO_FIELD_HEIGHT (vinfo) &&
        conn->modes[i].hdisplay == GST_VIDEO_INFO_WIDTH (vinfo)) {
      vrefresh = conn->modes[i].clock * 1000.00 /
          (conn->modes[i].htotal * conn->modes[i].vtotal);
      if (GST_VIDEO_INFO_INTERLACE_MODE (vinfo) ==
          GST_VIDEO_INTERLACE_MODE_ALTERNATE) {

        if (!(conn->modes[i].flags & DRM_MODE_FLAG_INTERLACE))
          continue;

        if (ABS (vrefresh - fps) > 0.005)
          continue;
      } else if (ABS (vrefresh - fps) > 0.005) {
        cached_mode = &conn->modes[i];
        continue;
      }
      mode = &conn->modes[i];
      break;
    }
  }
  if (!mode) {
    if (cached_mode)
      mode = cached_mode;
    else
      goto mode_failed;
  }

  err = drmModeSetCrtc (self->fd, self->crtc_id, fb_id, 0, 0,
      (uint32_t *) & self->conn_id, 1, mode);

/* Since at the moment force-modesetting doesn't support scaling */
  GST_OBJECT_LOCK (self);
  self->hdisplay = mode->hdisplay;
  self->vdisplay = mode->vdisplay;
  self->render_rect.x = 0;
  self->render_rect.y = 0;
  self->render_rect.w = self->hdisplay;
  self->render_rect.h = self->vdisplay;
  GST_OBJECT_UNLOCK (self);

  if (err)
    goto modesetting_failed;

  g_clear_pointer (&self->tmp_kmsmem, gst_memory_unref);
  self->tmp_kmsmem = mem;
  mem = NULL;
  self->vinfo_crtc = *vinfo;

  ret = TRUE;

bail:
  if (conn)
    drmModeFreeConnector (conn);
  if (mem)
    gst_memory_unref (mem);

  return ret;

  /* ERRORS */
bo_failed:
  {
    GST_ERROR_OBJECT (self,
        "failed to allocate buffer object for mode setting");
    goto bail;
  }
connector_failed:
  {
    GST_ERROR_OBJECT (self, "Could not find a valid monitor connector");
    goto bail;
  }
mode_failed:
  {
    GST_ERROR_OBJECT (self, "cannot find appropriate mode");
    goto bail;
  }
modesetting_failed:
  {
    GST_ERROR_OBJECT (self, "Failed to set mode: %s", g_strerror (errno));
    goto bail;
  }
}

static gboolean
set_crtc_to_plane_size (GstKMSSink * self, GstVideoInfo * vinfo)
{
  const gchar *format;
  gint j;
  GstVideoFormat fmt;
  gboolean ret = FALSE;
  GstVideoInfo vinfo_crtc;
  drmModePlane *primary_plane;

  if (self->primary_plane_id != -1)
    primary_plane = drmModeGetPlane (self->fd, self->primary_plane_id);
  if (!primary_plane)
    return ret;

  if (self->primary_plane_id != -1)
    ret =
        set_property_value_for_plane_id (self->fd, self->primary_plane_id,
        "alpha", 0);
  if (!ret)
    GST_ERROR_OBJECT (self, "Unable to reset alpha value of base plane");

  for (j = 0; j < primary_plane->count_formats; j++) {
    fmt = gst_video_format_from_drm (primary_plane->formats[j]);
    if (fmt == GST_VIDEO_FORMAT_UNKNOWN) {
      GST_INFO_OBJECT (self, "ignoring format %" GST_FOURCC_FORMAT,
          GST_FOURCC_ARGS (primary_plane->formats[j]));
      continue;
    } else {
      break;
    }
  }
  if (primary_plane)
    drmModeFreePlane (primary_plane);

  gst_video_info_set_interlaced_format (&vinfo_crtc, fmt,
      GST_VIDEO_INFO_INTERLACE_MODE (vinfo), vinfo->width, vinfo->height);
  GST_VIDEO_INFO_FPS_N (&vinfo_crtc) = GST_VIDEO_INFO_FPS_N (vinfo);
  GST_VIDEO_INFO_FPS_D (&vinfo_crtc) = GST_VIDEO_INFO_FPS_D (vinfo);
  format = gst_video_format_to_string (vinfo_crtc.finfo->format);

  GST_DEBUG_OBJECT (self,
      "Format for modesetting = %s, width = %d and height = %d", format,
      vinfo->width, vinfo->height);
  ret = configure_mode_setting (self, &vinfo_crtc);

  return ret;
}

static gboolean
ensure_allowed_caps (GstKMSSink * self, drmModeConnector * conn,
    drmModePlane * plane, drmModeRes * res)
{
  GstCaps *out_caps, *tmp_caps, *caps;
  int i, j;
  GstVideoFormat fmt;
  const gchar *format;
  drmModeModeInfo *mode;
  gint count_modes;

  if (self->allowed_caps)
    return TRUE;

  out_caps = gst_caps_new_empty ();
  if (!out_caps)
    return FALSE;

  if (conn && self->modesetting_enabled)
    count_modes = conn->count_modes;
  else
    count_modes = 1;

  for (i = 0; i < count_modes; i++) {
    tmp_caps = gst_caps_new_empty ();
    if (!tmp_caps)
      return FALSE;

    mode = NULL;
    if (conn && self->modesetting_enabled)
      mode = &conn->modes[i];

    for (j = 0; j < plane->count_formats; j++) {
      if (self->gray_to_yuv444) {
        if (plane->formats[j] == DRM_FORMAT_YUV444)
          plane->formats[j] = DRM_FORMAT_Y8;
        if (plane->formats[j] == DRM_FORMAT_X403)
          plane->formats[j] = DRM_FORMAT_Y10;
      }
      fmt = gst_video_format_from_drm (plane->formats[j]);
      if (fmt == GST_VIDEO_FORMAT_UNKNOWN) {
        GST_INFO_OBJECT (self, "ignoring format %" GST_FOURCC_FORMAT,
            GST_FOURCC_ARGS (plane->formats[j]));
        continue;
      }

      format = gst_video_format_to_string (fmt);

      if (mode) {
        gboolean interlaced;
        gint height;

        height = mode->vdisplay;
        interlaced = (conn->modes[i].flags & DRM_MODE_FLAG_INTERLACE);

        if (interlaced)
          /* Expose the frame height in caps, not the field */
          height *= 2;

        if (self->gray_to_yuv444)
          height = 3 * height;

        caps = gst_caps_new_simple ("video/x-raw",
            "format", G_TYPE_STRING, format,
            "width", G_TYPE_INT, mode->hdisplay,
            "height", G_TYPE_INT, height,
            "framerate", GST_TYPE_FRACTION_RANGE, 0, 1, G_MAXINT, 1, NULL);

        if (interlaced) {
          GstCapsFeatures *feat;

          feat =
              gst_caps_features_new (GST_CAPS_FEATURE_FORMAT_INTERLACED, NULL);
          gst_caps_set_features (caps, 0, feat);
        }
      } else {
        GstStructure *s;
        GstCapsFeatures *feat;

        s = gst_structure_new ("video/x-raw",
            "format", G_TYPE_STRING, format,
            "width", GST_TYPE_INT_RANGE, res->min_width, res->max_width,
            "height", GST_TYPE_INT_RANGE, res->min_height, res->max_height,
            "framerate", GST_TYPE_FRACTION_RANGE, 0, 1, G_MAXINT, 1, NULL);

        /* FIXME: How could we check if res supports interlacing? */
        caps = gst_caps_new_full (s, gst_structure_copy (s), NULL);

        feat = gst_caps_features_new (GST_CAPS_FEATURE_FORMAT_INTERLACED, NULL);
        gst_caps_set_features (caps, 1, feat);
      }
      if (!caps)
        continue;

      tmp_caps = gst_caps_merge (tmp_caps, caps);
    }

    out_caps = gst_caps_merge (out_caps, gst_caps_simplify (tmp_caps));
  }

  if (gst_caps_is_empty (out_caps)) {
    GST_DEBUG_OBJECT (self, "allowed caps is empty");
    gst_caps_unref (out_caps);
    return FALSE;
  }

  out_caps = gst_kms_add_xlnx_ll_caps (out_caps, TRUE);

  self->allowed_caps = gst_caps_simplify (out_caps);

  GST_DEBUG_OBJECT (self, "allowed caps = %" GST_PTR_FORMAT,
      self->allowed_caps);

  return TRUE;
}

static gboolean
set_drm_property (gint fd, guint32 object, guint32 object_type,
    drmModeObjectPropertiesPtr properties, const gchar * prop_name,
    guint64 value)
{
  guint i;
  gboolean ret = FALSE;

  for (i = 0; i < properties->count_props && !ret; i++) {
    drmModePropertyPtr property;

    property = drmModeGetProperty (fd, properties->props[i]);

    /* GstStructure parser limits the set of supported character, so we
     * replace the invalid characters with '-'. In DRM, this is generally
     * replacing spaces into '-'. */
    g_strcanon (property->name, G_CSET_a_2_z G_CSET_A_2_Z G_CSET_DIGITS "_",
        '-');

    GST_LOG ("found property %s (looking for %s)", property->name, prop_name);

    if (!strcmp (property->name, prop_name)) {
      drmModeObjectSetProperty (fd, object, object_type,
          property->prop_id, value);
      ret = TRUE;
    }
    drmModeFreeProperty (property);
  }

  return ret;
}

typedef struct
{
  GstKMSSink *self;
  drmModeObjectPropertiesPtr properties;
  guint obj_id;
  guint obj_type;
  const gchar *obj_type_str;
} SetPropsIter;

static gboolean
set_obj_prop (GQuark field_id, const GValue * value, gpointer user_data)
{
  SetPropsIter *iter = user_data;
  GstKMSSink *self = iter->self;
  const gchar *name;
  guint64 v;

  name = g_quark_to_string (field_id);

  if (G_VALUE_HOLDS (value, G_TYPE_INT))
    v = g_value_get_int (value);
  else if (G_VALUE_HOLDS (value, G_TYPE_UINT))
    v = g_value_get_uint (value);
  else if (G_VALUE_HOLDS (value, G_TYPE_INT64))
    v = g_value_get_int64 (value);
  else if (G_VALUE_HOLDS (value, G_TYPE_UINT64))
    v = g_value_get_uint64 (value);
  else {
    GST_WARNING_OBJECT (self,
        "'uint64' value expected for control '%s'.", name);
    return TRUE;
  }

  if (set_drm_property (self->fd, iter->obj_id, iter->obj_type,
          iter->properties, name, v)) {
    GST_DEBUG_OBJECT (self,
        "Set %s property '%s' to %" G_GUINT64_FORMAT,
        iter->obj_type_str, name, v);
  } else {
    GST_WARNING_OBJECT (self,
        "Failed to set %s property '%s' to %" G_GUINT64_FORMAT,
        iter->obj_type_str, name, v);
  }

  return TRUE;
}

static void
gst_kms_sink_update_properties (SetPropsIter * iter, GstStructure * props)
{
  GstKMSSink *self = iter->self;

  iter->properties = drmModeObjectGetProperties (self->fd, iter->obj_id,
      iter->obj_type);

  gst_structure_foreach (props, set_obj_prop, iter);

  drmModeFreeObjectProperties (iter->properties);
}

static void
gst_kms_sink_update_connector_properties (GstKMSSink * self)
{
  SetPropsIter iter;

  if (!self->connector_props)
    return;

  iter.self = self;
  iter.obj_id = self->conn_id;
  iter.obj_type = DRM_MODE_OBJECT_CONNECTOR;
  iter.obj_type_str = "connector";

  gst_kms_sink_update_properties (&iter, self->connector_props);
}

static void
gst_kms_sink_update_plane_properties (GstKMSSink * self)
{
  SetPropsIter iter;

  if (!self->plane_props)
    return;

  iter.self = self;
  iter.obj_id = self->plane_id;
  iter.obj_type = DRM_MODE_OBJECT_PLANE;
  iter.obj_type_str = "plane";

  gst_kms_sink_update_properties (&iter, self->plane_props);
}


static void
gst_kms_sink_get_times (GstBaseSink * bsink, GstBuffer * buffer,
    GstClockTime * start, GstClockTime * end)
{
  GstKMSSink *self;
  GstClockTime timestamp, duration;
  GstClockTimeDiff vblank_diff = 0, vblank_drift = 0;
  GstClockTimeDiff ts_diff = 0, ts_drift = 0;

  self = GST_KMS_SINK (bsink);
  timestamp = GST_BUFFER_PTS (buffer);
  if (GST_CLOCK_TIME_IS_VALID (timestamp))
    *start = timestamp;
  else
    return;

  if (self->last_ts == timestamp || !self->do_timestamp) {
    GST_TRACE_OBJECT (self,
        "self->last_ts: %" GST_TIME_FORMAT " self->do_timestamp %d",
        GST_TIME_ARGS (self->last_ts), self->do_timestamp);
    goto done;
  }

  GST_TRACE_OBJECT (self,
      "original ts :%" GST_TIME_FORMAT " last_orig_ts :%" GST_TIME_FORMAT
      " last_ts :%" GST_TIME_FORMAT, GST_TIME_ARGS (timestamp),
      GST_TIME_ARGS (self->last_orig_ts), GST_TIME_ARGS (self->last_ts));

  if (GST_CLOCK_TIME_IS_VALID (self->prev_last_vblank)
      && GST_CLOCK_TIME_IS_VALID (self->last_ts)) {
    vblank_diff = GST_CLOCK_DIFF (self->prev_last_vblank, self->last_vblank);
    vblank_drift =
        ABS (GST_CLOCK_DIFF (GST_BUFFER_DURATION (buffer), vblank_diff));
    ts_diff = GST_CLOCK_DIFF (self->last_orig_ts, timestamp);
    ts_drift = ABS (GST_CLOCK_DIFF (GST_BUFFER_DURATION (buffer), ts_diff));
    GST_TRACE_OBJECT (self, "vblank_diff: %" GST_TIME_FORMAT
        ", vblank_drift: %" GST_TIME_FORMAT ", ts_diff :%" GST_TIME_FORMAT
        ", ts_drift %" GST_TIME_FORMAT, GST_TIME_ARGS (vblank_diff),
        GST_TIME_ARGS (vblank_drift), GST_TIME_ARGS (ts_diff),
        GST_TIME_ARGS (ts_drift));

    if (ts_drift < 2 * GST_MSECOND && vblank_drift < 2 * GST_MSECOND) {
      *start = self->last_ts + vblank_diff;
      *end = *start + duration;
      GST_DEBUG_OBJECT (self, "got start: %" GST_TIME_FORMAT
          ", adjusted: %" GST_TIME_FORMAT ", delta %" GST_TIME_FORMAT,
          GST_TIME_ARGS (timestamp), GST_TIME_ARGS (*start),
          GST_TIME_ARGS ((GST_CLOCK_DIFF (timestamp, *start))));
    } else {
      if (ts_drift > 2 * GST_MSECOND) {
        self->prev_last_vblank = GST_CLOCK_TIME_NONE;
        self->last_vblank = GST_CLOCK_TIME_NONE;
        GST_DEBUG_OBJECT (self, "Need resyncing as packet loss happen");
      }
      *start = self->last_ts + ts_diff;
      *end = *start + duration;
      GST_DEBUG_OBJECT (self, "got start: %" GST_TIME_FORMAT
          ", gap found, adjusted : to %" GST_TIME_FORMAT " as per ts, delta %"
          GST_TIME_FORMAT ", ts_diff %" GST_TIME_FORMAT "vblank_diff %"
          GST_TIME_FORMAT ", ts_drift %" GST_TIME_FORMAT ", vsync_drift %"
          GST_TIME_FORMAT, GST_TIME_ARGS (timestamp), GST_TIME_ARGS (*start),
          GST_TIME_ARGS ((GST_CLOCK_DIFF (timestamp, *start))),
          GST_TIME_ARGS (ts_diff), GST_TIME_ARGS (vblank_diff),
          GST_TIME_ARGS (ts_drift), GST_TIME_ARGS (vblank_drift));
    }
  }

  GST_BUFFER_PTS (buffer) = *start;

  self->last_orig_ts = timestamp;
  self->last_ts = *start;
done:
  duration = GST_BUFFER_DURATION (buffer);
  if (GST_CLOCK_TIME_IS_VALID (duration)) {
    *end = *start + duration;
  }
  GST_LOG_OBJECT (self, "got times start: %" GST_TIME_FORMAT
      ", stop: %" GST_TIME_FORMAT, GST_TIME_ARGS (*start),
      GST_TIME_ARGS (*end));
}

static gboolean
gst_kms_sink_start (GstBaseSink * bsink)
{
  GstKMSSink *self;
  drmModeRes *res;
  drmModeConnector *conn;
  drmModeCrtc *crtc;
  drmModePlaneRes *pres;
  drmModePlane *plane = NULL, *primary_plane = NULL;
  gboolean universal_planes;
  gboolean ret;
  gint plane_type = -1;

  self = GST_KMS_SINK (bsink);
  universal_planes = FALSE;
  ret = FALSE;
  res = NULL;
  conn = NULL;
  crtc = NULL;
  pres = NULL;
  plane = NULL;

  self->xlnx_ll = FALSE;
  self->primary_plane_id = -1;

  if (self->devname || self->bus_id)
    self->fd = drmOpen (self->devname, self->bus_id);
  else
    self->fd = kms_open (&self->devname);
  if (self->fd < 0)
    goto open_failed;

  log_drm_version (self);
  if (!get_drm_caps (self))
    goto bail;

  res = drmModeGetResources (self->fd);
  if (!res)
    goto resources_failed;

  if (self->conn_id == -1)
    conn = find_main_monitor (self->fd, res);
  else
    conn = drmModeGetConnector (self->fd, self->conn_id);
  if (!conn)
    goto connector_failed;

  crtc = find_crtc_for_connector (self->fd, res, conn, &self->pipe);

  if (!crtc)
    goto crtc_failed;

  if ((!crtc->mode_valid || self->modesetting_enabled)
      && !self->fullscreen_enabled) {
    GST_DEBUG_OBJECT (self, "enabling modesetting");
    self->modesetting_enabled = TRUE;
    universal_planes = TRUE;
  }

  if (crtc->mode_valid && self->modesetting_enabled && self->restore_crtc) {
    self->saved_crtc = (drmModeCrtc *) crtc;
  }

  if (self->fullscreen_enabled) {
    universal_planes = TRUE;
    plane_type = DRM_PLANE_TYPE_OVERLAY;
  }

retry_find_plane:
  if (universal_planes &&
      drmSetClientCap (self->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1))
    goto set_cap_failed;

  pres = drmModeGetPlaneResources (self->fd);
  if (!pres)
    goto plane_resources_failed;

  if (self->plane_id == -1)
    plane =
        find_plane_for_crtc (self->fd, res, pres, crtc->crtc_id, plane_type);
  else
    plane = drmModeGetPlane (self->fd, self->plane_id);

  if (!plane)
    goto plane_failed;

  primary_plane =
      find_plane_for_crtc (self->fd, res, pres, crtc->crtc_id,
      DRM_PLANE_TYPE_PRIMARY);
  if (!primary_plane && self->fullscreen_enabled)
    goto primary_plane_failed;
  if (primary_plane)
    self->primary_plane_id = primary_plane->plane_id;

  if (self->fullscreen_enabled == 1)
    self->saved_crtc = (drmModeCrtc *) crtc;

  if (!ensure_allowed_caps (self, conn, plane, res))
    goto allowed_caps_failed;

  self->conn_id = conn->connector_id;
  self->crtc_id = crtc->crtc_id;
  self->plane_id = plane->plane_id;

  GST_INFO_OBJECT (self, "connector id = %d / crtc id = %d / plane id = %d",
      self->conn_id, self->crtc_id, self->plane_id);

  GST_OBJECT_LOCK (self);
  self->hdisplay = crtc->mode.hdisplay;
  self->vdisplay = crtc->mode.vdisplay;

  if (self->render_rect.w == 0 || self->render_rect.h == 0) {
    self->render_rect.x = 0;
    self->render_rect.y = 0;
    self->render_rect.w = self->hdisplay;
    self->render_rect.h = self->vdisplay;
  }

  self->pending_rect = self->render_rect;
  GST_OBJECT_UNLOCK (self);

  self->buffer_id = crtc->buffer_id;

  if (self->avoid_field_inversion)
    self->hold_extra_sample = TRUE;

  self->mm_width = conn->mmWidth;
  self->mm_height = conn->mmHeight;

  GST_INFO_OBJECT (self, "display size: pixels = %dx%d / millimeters = %dx%d",
      self->hdisplay, self->vdisplay, self->mm_width, self->mm_height);

  self->pollfd.fd = self->fd;
  gst_poll_add_fd (self->poll, &self->pollfd);
  gst_poll_fd_ctl_read (self->poll, &self->pollfd, TRUE);

  g_object_notify_by_pspec (G_OBJECT (self), g_properties[PROP_DISPLAY_WIDTH]);
  g_object_notify_by_pspec (G_OBJECT (self), g_properties[PROP_DISPLAY_HEIGHT]);

  ret = TRUE;

bail:
  if (plane)
    drmModeFreePlane (plane);
  if (primary_plane)
    drmModeFreePlane (primary_plane);
  if (pres)
    drmModeFreePlaneResources (pres);
  if (crtc != self->saved_crtc && !self->fullscreen_enabled)
    drmModeFreeCrtc (crtc);
  if (conn)
    drmModeFreeConnector (conn);
  if (res)
    drmModeFreeResources (res);

  if (!ret && self->fd >= 0) {
    drmClose (self->fd);
    self->fd = -1;
  }

  return ret;

  /* ERRORS */
open_failed:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, OPEN_READ_WRITE,
        ("Could not open DRM module %s", GST_STR_NULL (self->devname)),
        ("reason: %s (%d)", g_strerror (errno), errno));
    return FALSE;
  }

resources_failed:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS,
        ("drmModeGetResources failed"),
        ("reason: %s (%d)", g_strerror (errno), errno));
    goto bail;
  }

connector_failed:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS,
        ("Could not find a valid monitor connector"), (NULL));
    goto bail;
  }

crtc_failed:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS,
        ("Could not find a crtc for connector"), (NULL));
    goto bail;
  }

set_cap_failed:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS,
        ("Could not set universal planes capability bit"), (NULL));
    goto bail;
  }

plane_resources_failed:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS,
        ("drmModeGetPlaneResources failed"),
        ("reason: %s (%d)", g_strerror (errno), errno));
    goto bail;
  }

plane_failed:
  {
    if (universal_planes) {
      GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS,
          ("Could not find a plane for crtc"), (NULL));
      goto bail;
    } else {
      universal_planes = TRUE;
      goto retry_find_plane;
    }
  }

primary_plane_failed:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS,
        ("Could not find primary plane for crtc"), (NULL));
    goto bail;
  }
allowed_caps_failed:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS,
        ("Could not get allowed GstCaps of device"),
        ("driver does not provide mode settings configuration"));
    goto bail;
  }
}

static gboolean
gst_kms_sink_stop (GstBaseSink * bsink)
{
  GstKMSSink *self;
  int err;

  self = GST_KMS_SINK (bsink);

  if (self->allocator)
    gst_kms_allocator_clear_cache (self->allocator);

  if (self->fullscreen_enabled && self->primary_plane_id != -1) {
    err = set_property_value_for_plane_id (self->fd, self->primary_plane_id,
        "alpha", 255);
    if (!err)
      GST_ERROR_OBJECT (self, "Unable to reset alpha value of primary plane");
  }

  gst_buffer_replace (&self->last_buffer, NULL);
  if (self->hold_extra_sample)
    gst_buffer_replace (&self->previous_last_buffer, NULL);
  gst_caps_replace (&self->allowed_caps, NULL);
  gst_object_replace ((GstObject **) & self->pool, NULL);
  gst_object_replace ((GstObject **) & self->allocator, NULL);

  gst_poll_remove_fd (self->poll, &self->pollfd);
  gst_poll_restart (self->poll);
  gst_poll_fd_init (&self->pollfd);


  g_clear_pointer (&self->tmp_kmsmem, gst_memory_unref);

  if (self->saved_crtc) {
    drmModeCrtc *crtc = (drmModeCrtc *) self->saved_crtc;

    err = drmModeSetCrtc (self->fd, crtc->crtc_id, crtc->buffer_id, crtc->x,
        crtc->y, (uint32_t *) & self->conn_id, 1, &crtc->mode);
    if (err)
      GST_ERROR_OBJECT (self, "Failed to restore previous CRTC mode: %s",
          g_strerror (errno));

    drmModeFreeCrtc (crtc);
    self->saved_crtc = NULL;
  }

  if (self->fd >= 0) {
    drmClose (self->fd);
    self->fd = -1;
  }

  GST_OBJECT_LOCK (bsink);
  self->hdisplay = 0;
  self->vdisplay = 0;
  self->pending_rect.x = 0;
  self->pending_rect.y = 0;
  self->pending_rect.w = 0;
  self->pending_rect.h = 0;
  self->render_rect = self->pending_rect;
  self->primary_plane_id = -1;
  GST_OBJECT_UNLOCK (bsink);

  g_object_notify_by_pspec (G_OBJECT (self), g_properties[PROP_DISPLAY_WIDTH]);
  g_object_notify_by_pspec (G_OBJECT (self), g_properties[PROP_DISPLAY_HEIGHT]);

  return TRUE;
}

static GstCaps *
gst_kms_sink_get_allowed_caps (GstKMSSink * self)
{
  if (!self->allowed_caps)
    return NULL;                /* base class will return the template caps */
  return gst_caps_ref (self->allowed_caps);
}

static GstCaps *
gst_kms_sink_get_caps (GstBaseSink * bsink, GstCaps * filter)
{
  GstKMSSink *self;
  GstCaps *caps, *out_caps;
  GstStructure *s;
  guint dpy_par_n, dpy_par_d;

  self = GST_KMS_SINK (bsink);

  caps = gst_kms_sink_get_allowed_caps (self);
  if (!caps)
    return NULL;

  GST_OBJECT_LOCK (self);

  if (self->gray_to_yuv444) {
    int i;
    gint min, max;
    GValue *h = { 0, };

    out_caps = gst_caps_new_empty ();

    for (i = 0; i < gst_caps_get_size (caps); i++) {
      s = gst_structure_copy (gst_caps_get_structure (caps, i));
      h = gst_structure_get_value (s, "height");
      if (GST_VALUE_HOLDS_INT_RANGE (h)) {
        min = gst_value_get_int_range_min (h);
        max = gst_value_get_int_range_max (h);
        if (max < GRAY_HEIGHT_MAX)
          max = GRAY_HEIGHT_MAX;
        gst_structure_set (s, "height", GST_TYPE_INT_RANGE, min, max, NULL);
      } else {
        gst_structure_set (s, "height", G_TYPE_INT, GRAY_HEIGHT_MAX, NULL);
      }
      gst_caps_append_structure (out_caps, s);
    }

    out_caps = gst_caps_merge (out_caps, caps);
    caps = out_caps;
  }

  if (!self->can_scale) {
    out_caps = gst_caps_new_empty ();
    gst_video_calculate_device_ratio (self->hdisplay, self->vdisplay,
        self->mm_width, self->mm_height, &dpy_par_n, &dpy_par_d);

    s = gst_structure_copy (gst_caps_get_structure (caps, 0));
    gst_structure_set (s, "width", G_TYPE_INT, self->pending_rect.w,
        "height", G_TYPE_INT, self->pending_rect.h, NULL);

    gst_caps_append_structure (out_caps, s);

    out_caps = gst_caps_merge (out_caps, caps);
    caps = NULL;

  } else {
    out_caps = gst_caps_make_writable (caps);
    caps = NULL;
  }

  GST_OBJECT_UNLOCK (self);

  GST_DEBUG_OBJECT (self, "Proposing caps %" GST_PTR_FORMAT, out_caps);

  if (filter) {
    caps = out_caps;
    out_caps = gst_caps_intersect_full (caps, filter, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (caps);
  }

  return out_caps;
}

static GstBufferPool *
gst_kms_sink_create_pool (GstKMSSink * self, GstCaps * caps, gsize size,
    gint min)
{
  GstBufferPool *pool;
  GstStructure *config;

  pool = gst_kms_buffer_pool_new ();
  if (!pool)
    goto pool_failed;

  config = gst_buffer_pool_get_config (pool);
  gst_buffer_pool_config_set_params (config, caps, size, min, 0);
  gst_buffer_pool_config_add_option (config, GST_BUFFER_POOL_OPTION_VIDEO_META);

  ensure_kms_allocator (self);
  gst_buffer_pool_config_set_allocator (config, self->allocator, NULL);

  if (!gst_buffer_pool_set_config (pool, config))
    goto config_failed;

  return pool;

  /* ERRORS */
pool_failed:
  {
    GST_ERROR_OBJECT (self, "failed to create buffer pool");
    return NULL;
  }
config_failed:
  {
    GST_ERROR_OBJECT (self, "failed to set config");
    gst_object_unref (pool);
    return NULL;
  }
}

static gboolean
gst_kms_sink_calculate_display_ratio (GstKMSSink * self, GstVideoInfo * vinfo,
    gint * scaled_width, gint * scaled_height)
{
  guint dar_n, dar_d;
  guint video_width, video_height;
  guint video_par_n, video_par_d;
  guint dpy_par_n, dpy_par_d;

  video_width = GST_VIDEO_INFO_WIDTH (vinfo);
  video_height = GST_VIDEO_INFO_HEIGHT (vinfo);
  video_par_n = GST_VIDEO_INFO_PAR_N (vinfo);
  video_par_d = GST_VIDEO_INFO_PAR_D (vinfo);

  if (self->can_scale) {
    gst_video_calculate_device_ratio (self->hdisplay, self->vdisplay,
        self->mm_width, self->mm_height, &dpy_par_n, &dpy_par_d);
  } else {
    *scaled_width = video_width;
    *scaled_height = video_height;
    goto out;
  }

  if (!gst_video_calculate_display_ratio (&dar_n, &dar_d, video_width,
          video_height, video_par_n, video_par_d, dpy_par_n, dpy_par_d))
    return FALSE;

  GST_DEBUG_OBJECT (self, "video calculated display ratio: %d/%d", dar_n,
      dar_d);

  /* now find a width x height that respects this display ratio.
   * prefer those that have one of w/h the same as the incoming video
   * using wd / hd = dar_n / dar_d */

  /* start with same height, because of interlaced video */
  /* check hd / dar_d is an integer scale factor, and scale wd with the PAR */
  if (video_height % dar_d == 0) {
    GST_DEBUG_OBJECT (self, "keeping video height");
    *scaled_width = (guint)
        gst_util_uint64_scale_int (video_height, dar_n, dar_d);
    *scaled_height = video_height;
  } else if (video_width % dar_n == 0) {
    GST_DEBUG_OBJECT (self, "keeping video width");
    *scaled_width = video_width;
    *scaled_height = (guint)
        gst_util_uint64_scale_int (video_width, dar_d, dar_n);
  } else {
    GST_DEBUG_OBJECT (self, "approximating while keeping video height");
    *scaled_width = (guint)
        gst_util_uint64_scale_int (video_height, dar_n, dar_d);
    *scaled_height = video_height;
  }

out:
  GST_DEBUG_OBJECT (self, "scaling to %dx%d", *scaled_width, *scaled_height);

  return TRUE;
}

static gint
gst_kms_sink_hdr_set_metadata (GstKMSSink * self, GstCaps * caps, guint32 * id)
{
#ifdef HAVE_HDR_OUTPUT_METADATA
  gint ret = 0;
  GstVideoMasteringDisplayInfo minfo;
  GstVideoContentLightLevel cinfo;
  struct hdr_metadata_infoframe *hdr_infoframe;
#ifdef HAVE_GEN_HDR_OUTPUT_METADATA
  struct gen_hdr_output_metadata hdr_metadata = { 0 };
  const char prop_name[] = "GEN_HDR_OUTPUT_METADATA";
  hdr_infoframe = (struct hdr_metadata_infoframe *) hdr_metadata.payload;
#else
  const char prop_name[] = "HDR_OUTPUT_METADATA";
  hdr_infoframe = g_new (struct hdr_metadata_infoframe, 1);
#endif

  gst_video_mastering_display_info_init (&minfo);
  gst_video_content_light_level_init (&cinfo);

  if (gst_video_colorimetry_matches (&self->vinfo.colorimetry,
          GST_VIDEO_COLORIMETRY_BT2100_PQ)
      || gst_video_colorimetry_matches (&self->vinfo.colorimetry,
          GST_VIDEO_COLORIMETRY_BT2100_HLG)) {
    int i;

#ifdef HAVE_GEN_HDR_OUTPUT_METADATA
    hdr_metadata.metadata_type = DRM_HDR_TYPE_HDR10;
    hdr_metadata.size = sizeof (struct hdr_metadata_infoframe);
#endif
    hdr_infoframe->metadata_type = DRM_STATIC_METADATA_TYPE1;
    if (gst_video_colorimetry_matches (&self->vinfo.colorimetry,
            GST_VIDEO_COLORIMETRY_BT2100_PQ))
      hdr_infoframe->eotf = DRM_EOTF_SMPTE_ST2084;
    else
      hdr_infoframe->eotf = DRM_EOTF_BT_2100_HLG;

    GST_LOG_OBJECT (self, "Setting EOTF to: %u", hdr_infoframe->eotf);

    if (gst_video_mastering_display_info_from_caps (&minfo, caps)) {
      for (i = 0; i < 3; i++) {
        hdr_infoframe->display_primaries[i].x = minfo.display_primaries[i].x;
        hdr_infoframe->display_primaries[i].y = minfo.display_primaries[i].y;
      }

      hdr_infoframe->white_point.x = minfo.white_point.x;
      hdr_infoframe->white_point.y = minfo.white_point.y;
      /* CTA 861.G is 1 candelas per square metre (cd/m^2) while
       * GstVideoMasteringDisplayInfo is 0.0001 cd/m^2 */
      hdr_infoframe->max_display_mastering_luminance =
          minfo.max_display_mastering_luminance / 10000;
      hdr_infoframe->min_display_mastering_luminance =
          minfo.min_display_mastering_luminance;
      GST_LOG_OBJECT (self, "Setting mastering display info: "
          "Red(%u, %u) "
          "Green(%u, %u) "
          "Blue(%u, %u) "
          "White(%u, %u) "
          "max_luminance(%u) "
          "min_luminance(%u) ",
          minfo.display_primaries[0].x,
          minfo.display_primaries[0].y,
          minfo.display_primaries[1].x,
          minfo.display_primaries[1].y,
          minfo.display_primaries[2].x,
          minfo.display_primaries[2].y, minfo.white_point.x,
          minfo.white_point.y,
          minfo.max_display_mastering_luminance,
          minfo.min_display_mastering_luminance);
    }

    if (gst_video_content_light_level_from_caps (&cinfo, caps)) {
      hdr_infoframe->max_cll = cinfo.max_content_light_level;
      hdr_infoframe->max_fall = cinfo.max_frame_average_light_level;
      GST_LOG_OBJECT (self, "Setting content light level: "
          "maxCLL:(%u), maxFALL:(%u)",
          cinfo.max_content_light_level, cinfo.max_frame_average_light_level);
    }
  }
#ifdef HAVE_GEN_HDR_OUTPUT_METADATA
  ret =
      drmModeCreatePropertyBlob (self->fd, &hdr_metadata,
      sizeof (struct gen_hdr_output_metadata), id);
#else
  ret =
      drmModeCreatePropertyBlob (self->fd, hdr_infoframe,
      sizeof (struct hdr_metadata_infoframe), id);
#endif
  if (ret) {
    GST_WARNING_OBJECT (self, "drmModeCreatePropertyBlob failed: %s (%d)",
        strerror (-ret), ret);
  } else {
    if (!self->connector_props)
      self->connector_props =
          gst_structure_new ("connector-props", prop_name,
          G_TYPE_INT64, *id, NULL);
    else
      gst_structure_set (self->connector_props, prop_name,
          G_TYPE_INT64, *id, NULL);
  }

#ifndef HAVE_GEN_HDR_OUTPUT_METADATA
  g_free (hdr_infoframe);
#endif

  return ret;
#endif /* HAVE_HDR_OUTPUT_METADATA */
}

static gboolean
gst_kms_sink_set_caps (GstBaseSink * bsink, GstCaps * caps)
{
  GstKMSSink *self;
  GstVideoInfo vinfo;
  gint fps_n, fps_d;
  guint32 hdr_id;
  gint ret = 0;

  self = GST_KMS_SINK (bsink);

  if (!gst_video_info_from_caps (&vinfo, caps))
    goto invalid_format;

  if (self->gray_to_yuv444) {
    fps_n = vinfo.fps_n;
    fps_d = vinfo.fps_d;
    if (vinfo.finfo->format == GST_VIDEO_FORMAT_GRAY8)
      gst_video_info_set_format (&vinfo, GST_VIDEO_FORMAT_Y444, vinfo.width,
          vinfo.height / 3);
    else if (vinfo.finfo->format == GST_VIDEO_FORMAT_GRAY10_LE32)
      gst_video_info_set_format (&vinfo, GST_VIDEO_FORMAT_Y444_10LE32,
          vinfo.width, vinfo.height / 3);

    vinfo.fps_n = fps_n;
    vinfo.fps_d = fps_d;
  }

  /* on the first set_caps self->vinfo is not initialized, yet. */
  if (self->vinfo.finfo->format != GST_VIDEO_FORMAT_UNKNOWN)
    self->last_vinfo = self->vinfo;
  else
    self->last_vinfo = vinfo;
  self->vinfo = vinfo;

  if (!gst_kms_sink_calculate_display_ratio (self, &vinfo,
          &GST_VIDEO_SINK_WIDTH (self), &GST_VIDEO_SINK_HEIGHT (self)))
    goto no_disp_ratio;

  if (GST_VIDEO_SINK_WIDTH (self) <= 0 || GST_VIDEO_SINK_HEIGHT (self) <= 0)
    goto invalid_size;

  /* discard dumb buffer pool */
  if (self->pool) {
    gst_buffer_pool_set_active (self->pool, FALSE);
    gst_object_unref (self->pool);
    self->pool = NULL;
  }

  if (self->modesetting_enabled && !configure_mode_setting (self, &vinfo))
    goto modesetting_failed;

  if (self->fullscreen_enabled && !set_crtc_to_plane_size (self, &vinfo))
    goto modesetting_failed;

  if (!self->modesetting_enabled && !self->fullscreen_enabled &&
      GST_VIDEO_INFO_INTERLACE_MODE (&vinfo) ==
      GST_VIDEO_INTERLACE_MODE_ALTERNATE) {
    GST_DEBUG_OBJECT (self,
        "configure mode setting as input is in alternate interlacing mode");
    if (!configure_mode_setting (self, &vinfo))
      goto modesetting_failed;
  }

  GST_OBJECT_LOCK (self);
  if (self->reconfigure) {
    self->reconfigure = FALSE;
    self->render_rect = self->pending_rect;
  }
  GST_OBJECT_UNLOCK (self);

  {
    GstCapsFeatures *features;

    features = gst_caps_get_features (caps, 0);
    if (features
        && gst_caps_features_contains (features,
            GST_CAPS_FEATURE_MEMORY_XLNX_LL)) {
      GST_DEBUG_OBJECT (self, "Input uses XLNX-LowLatency");
      self->xlnx_ll = TRUE;
    }
  }

  ret = gst_kms_sink_hdr_set_metadata (self, caps, &hdr_id);

  gst_kms_sink_update_connector_properties (self);
  gst_kms_sink_update_plane_properties (self);

  if (!ret) {
    ret = drmModeDestroyPropertyBlob (self->fd, hdr_id);
    if (ret)
      GST_WARNING_OBJECT (self, "drmModeDestroyPropertyBlob failed: %s (%d)",
          strerror (-ret), ret);
  }

  GST_DEBUG_OBJECT (self, "negotiated caps = %" GST_PTR_FORMAT, caps);

  return TRUE;

  /* ERRORS */
invalid_format:
  {
    GST_ERROR_OBJECT (self, "caps invalid");
    return FALSE;
  }

invalid_size:
  {
    GST_ELEMENT_ERROR (self, CORE, NEGOTIATION, (NULL),
        ("Invalid image size."));
    return FALSE;
  }

no_disp_ratio:
  {
    GST_ELEMENT_ERROR (self, CORE, NEGOTIATION, (NULL),
        ("Error calculating the output display ratio of the video."));
    return FALSE;
  }

modesetting_failed:
  {
    GST_ELEMENT_ERROR (self, CORE, NEGOTIATION, (NULL),
        ("failed to configure video mode"));
    return FALSE;
  }

}

static guint
get_padding_right (GstKMSSink * self, GstVideoInfo * info, guint pitch)
{
  guint padding_pixels = -1;
  guint plane_stride = GST_VIDEO_INFO_PLANE_STRIDE (info, 0);
  guint padding_bytes = pitch - plane_stride;

  switch (GST_VIDEO_INFO_FORMAT (info)) {
    case GST_VIDEO_FORMAT_NV12:
      padding_pixels = padding_bytes;
      break;
    case GST_VIDEO_FORMAT_RGBx:
    case GST_VIDEO_FORMAT_r210:
    case GST_VIDEO_FORMAT_Y410:
    case GST_VIDEO_FORMAT_BGRx:
    case GST_VIDEO_FORMAT_BGRA:
    case GST_VIDEO_FORMAT_RGBA:
      padding_pixels = padding_bytes / 4;
      break;
    case GST_VIDEO_FORMAT_YUY2:
    case GST_VIDEO_FORMAT_UYVY:
      padding_pixels = padding_bytes / 2;
      break;
    case GST_VIDEO_FORMAT_NV16:
      padding_pixels = padding_bytes;
      break;
    case GST_VIDEO_FORMAT_RGB:
    case GST_VIDEO_FORMAT_v308:
    case GST_VIDEO_FORMAT_BGR:
      padding_pixels = padding_bytes / 3;
      break;
    case GST_VIDEO_FORMAT_I422_10LE:
      padding_pixels = padding_bytes / 2;
      break;
    case GST_VIDEO_FORMAT_NV12_10LE32:
      padding_pixels = (padding_bytes * 3) / 4;
      break;
    case GST_VIDEO_FORMAT_GRAY8:
      padding_pixels = padding_bytes;
      break;
    case GST_VIDEO_FORMAT_GRAY10_LE32:
      padding_pixels = (padding_bytes * 3) / 4;
      break;
    case GST_VIDEO_FORMAT_I420:
      padding_pixels = padding_bytes;
      break;
    case GST_VIDEO_FORMAT_I420_10LE:
      padding_pixels = padding_bytes / 2;
      break;
    default:
      padding_pixels = -1;
  }
  return padding_pixels;
}

static gboolean
gst_kms_sink_propose_allocation (GstBaseSink * bsink, GstQuery * query)
{
  GstKMSSink *self;
  GstCaps *caps;
  gboolean need_pool;
  GstVideoInfo vinfo;
  GstBufferPool *pool;
  gsize size;
  gint ret, w, h, i;
  struct drm_mode_create_dumb arg = { 0, };
  guint32 fmt;
  drmModeConnector *conn;
  GstVideoAlignment align = { 0, };

  self = GST_KMS_SINK (bsink);

  GST_DEBUG_OBJECT (self, "propose allocation");

  gst_query_parse_allocation (query, &caps, &need_pool);
  if (!caps)
    goto no_caps;
  if (!gst_video_info_from_caps (&vinfo, caps))
    goto invalid_caps;

  conn = drmModeGetConnector (self->fd, self->conn_id);
  if (((self->devname ? !strcmp (self->devname, "xlnx") : 0)
          && conn->connector_type == DRM_MODE_CONNECTOR_DisplayPort)
      || (self->bus_id ? strstr (self->bus_id, "zynqmp-display") : 0)) {
    is_dp = TRUE;
    fmt = gst_drm_format_from_video (GST_VIDEO_INFO_FORMAT (&vinfo));
    arg.bpp = gst_drm_bpp_from_drm (fmt);
    w = GST_VIDEO_INFO_WIDTH (&vinfo);
    arg.width = gst_drm_width_from_drm (fmt, w);
    h = GST_VIDEO_INFO_FIELD_HEIGHT (&vinfo);
    arg.height = gst_drm_height_from_drm (fmt, h);

    ret = drmIoctl (self->fd, DRM_IOCTL_MODE_CREATE_DUMB, &arg);
    if (ret)
      goto no_pool;

    gst_video_alignment_reset (&align);
    align.padding_top = 0;
    align.padding_left = 0;
    align.padding_right = get_padding_right (self, &vinfo, arg.pitch);
    if (!arg.pitch || align.padding_right == -1) {
      align.padding_right = 0;
      for (i = 0; i < GST_VIDEO_INFO_N_PLANES (&vinfo); i++)
        align.stride_align[i] = 255;    /* 256-byte alignment */
    }
    align.padding_bottom = 0;
    gst_video_info_align (&vinfo, &align);

    GST_INFO_OBJECT (self, "padding_left %u, padding_right %u",
        align.padding_left, align.padding_right);
  }

  drmModeFreeConnector (conn);

  /* Update with the size use for display */
  size = GST_VIDEO_INFO_SIZE (&vinfo);

  GST_INFO_OBJECT (self, "size %lu", size);

  pool = NULL;
  if (need_pool) {
    pool = gst_kms_sink_create_pool (self, caps, size, 0);
    if (!pool)
      goto no_pool;

    /* Only export for pool used upstream */
    if (self->has_prime_export) {
      GstStructure *config = gst_buffer_pool_get_config (pool);
      gst_buffer_pool_config_add_option (config,
          GST_BUFFER_POOL_OPTION_KMS_PRIME_EXPORT);
      gst_buffer_pool_config_add_option (config,
          GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT);
      gst_buffer_pool_config_set_video_alignment (config, &align);
      gst_buffer_pool_set_config (pool, config);
    }
  }

  /* we need at least 2 buffer because we hold on to the last one */
  if (!self->hold_extra_sample)
    gst_query_add_allocation_pool (query, pool, size, 2, 0);
  else
    gst_query_add_allocation_pool (query, pool, size, 3, 0);

  if (pool)
    gst_object_unref (pool);

  gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL);
  gst_query_add_allocation_meta (query, GST_VIDEO_CROP_META_API_TYPE, NULL);

  return TRUE;

  /* ERRORS */
no_caps:
  {
    GST_DEBUG_OBJECT (bsink, "no caps specified");
    return FALSE;
  }
invalid_caps:
  {
    GST_DEBUG_OBJECT (bsink, "invalid caps specified");
    return FALSE;
  }
no_pool:
  {
    /* Already warned in create_pool */
    return FALSE;
  }
}

static void
sync_handler (gint fd, guint frame, guint sec, guint usec, gpointer data)
{
  gboolean *waiting;

  waiting = data;
  *waiting = FALSE;
}

static gboolean
gst_kms_sink_sync (GstKMSSink * self)
{
  gint ret;
  gboolean waiting;
  drmEventContext evctxt = {
    .version = DRM_EVENT_CONTEXT_VERSION,
    .page_flip_handler = sync_handler,
    .vblank_handler = sync_handler,
  };
  drmVBlank vbl = {
    .request = {
          .type = DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT,
          .sequence = 1,
          .signal = (gulong) & waiting,
        },
  };

  if (self->pipe == 1)
    vbl.request.type |= DRM_VBLANK_SECONDARY;
  else if (self->pipe > 1)
    vbl.request.type |= self->pipe << DRM_VBLANK_HIGH_CRTC_SHIFT;

  waiting = TRUE;
  if (!self->has_async_page_flip && !self->modesetting_enabled) {
    ret = drmWaitVBlank (self->fd, &vbl);
    if (ret)
      goto vblank_failed;
  } else {
    ret = drmModePageFlip (self->fd, self->crtc_id, self->buffer_id,
        DRM_MODE_PAGE_FLIP_EVENT, &waiting);
    if (ret)
      goto pageflip_failed;
  }

  while (waiting) {
    do {
      ret = gst_poll_wait (self->poll, 3 * GST_SECOND);
    } while (ret == -1 && (errno == EAGAIN || errno == EINTR));

    ret = drmHandleEvent (self->fd, &evctxt);
    if (ret)
      goto event_failed;
  }

  return TRUE;

  /* ERRORS */
vblank_failed:
  {
    GST_WARNING_OBJECT (self, "drmWaitVBlank failed: %s (%d)",
        g_strerror (errno), errno);
    return FALSE;
  }
pageflip_failed:
  {
    GST_WARNING_OBJECT (self, "drmModePageFlip failed: %s (%d)",
        g_strerror (errno), errno);
    return FALSE;
  }
event_failed:
  {
    GST_ERROR_OBJECT (self, "drmHandleEvent failed: %s (%d)",
        g_strerror (errno), errno);
    return FALSE;
  }
}

static gboolean
draw_rectangle (guint8 * chroma, roi_coordinate * roi, guint roi_cnt,
    guint frame_w, guint frame_h, guint stride, guint roi_rect_thickness,
    const GValue * roi_rect_yuv_color, GstVideoFormat format)
{
  guint i = 0, j = 0, k = 0, roi_ind = 0;
  guint x = 0, y = 0, w = 0, h = 0;
  guint8 *chroma_offset1, *chroma_offset2, *chroma_offset3;
  guint8 *h_chroma_offset1, *h_chroma_offset2, *h_chroma_offset3;
  guint8 *v_chroma_offset1, *v_chroma_offset2, *v_chroma_offset3;
  guint8 thickness = 0;
  gint u = 0, v = 0, vert_sampling = 0;

  for (roi_ind = 0; roi_ind < roi_cnt; roi_ind++) {
    /* Validate roi */
    if ((roi[roi_ind].xmin + roi[roi_ind].width) > frame_w) {
      roi[roi_ind].width = frame_w - roi[roi_ind].xmin;
    }
    if ((roi[roi_ind].ymin + roi[roi_ind].height) > frame_h) {
      roi[roi_ind].height = frame_h - roi[roi_ind].ymin;
    }
    if (((roi[roi_ind].xmin + roi[roi_ind].width) > frame_w) ||
        ((roi[roi_ind].ymin + roi[roi_ind].height) > frame_h)) {
      GST_WARNING
          ("skipping invalid roi xmin, ymin, width, height %d::%d::%d::%d",
          roi[roi_ind].xmin, roi[roi_ind].ymin, roi[roi_ind].width,
          roi[roi_ind].height);
      continue;
    }
    if ((0 == roi[roi_ind].width) || (0 == roi[roi_ind].height)) {
      GST_WARNING
          ("skipping invalid roi xmin, ymin, width, height %d::%d::%d::%d",
          roi[roi_ind].xmin, roi[roi_ind].ymin, roi[roi_ind].width,
          roi[roi_ind].height);
      continue;
    }

    /* Always start from first chroma component so make x even */
    x = ((roi[roi_ind].xmin & 0x1) ==
        0) ? (roi[roi_ind].xmin) : (roi[roi_ind].xmin - 1);
    y = roi[roi_ind].ymin;
    /* Always end with last chroma component so make width even */
    w = ((roi[roi_ind].width & 0x1) ==
        0) ? (roi[roi_ind].width) : (roi[roi_ind].width - 1);
    h = roi[roi_ind].height;

    if (format == GST_VIDEO_FORMAT_NV12)
      vert_sampling = 2;
    else if (format == GST_VIDEO_FORMAT_NV16)
      vert_sampling = 1;

    chroma_offset1 = chroma + ((y / vert_sampling) * stride) + x;
    chroma_offset2 = chroma_offset1 + ((h / vert_sampling) - 1) * stride;
    chroma_offset3 = chroma_offset1 + w - 2;

    v_chroma_offset1 = h_chroma_offset1 = chroma_offset1;
    v_chroma_offset2 = h_chroma_offset2 = chroma_offset2;
    v_chroma_offset3 = h_chroma_offset3 = chroma_offset3;

    if (gst_value_array_get_size (roi_rect_yuv_color) == 3) {
      u = g_value_get_int (gst_value_array_get_value (roi_rect_yuv_color, 1));
      v = g_value_get_int (gst_value_array_get_value (roi_rect_yuv_color, 2));
    }

    /* Draw horizontal lines */
    for (thickness = 0; thickness < roi_rect_thickness; thickness++) {
      for (k = 0; k < (2 / vert_sampling); k++) {
        for (i = 0; i < w; i = i + 2) {
          *(h_chroma_offset1 + i) = u;
          *(h_chroma_offset1 + i + 1) = v;
          *(h_chroma_offset2 + i) = u;
          *(h_chroma_offset2 + i + 1) = v;
        }
        /* To draw same line horizontally for NV16 format as no vertical subsampling */
        if (k < ((2 / vert_sampling) - 1)) {
          /* Increase h_chroma_offset1 by stride vertically */
          h_chroma_offset1 = h_chroma_offset1 + stride;
          /* Decrease h_chroma_offset2 by stride vertically */
          h_chroma_offset2 = h_chroma_offset2 - stride;
        }
      }
      /* Increase h_chroma_offset1 by stride vertically and by 2 horizontally */
      h_chroma_offset1 = h_chroma_offset1 + stride + 2;
      /* Decrease h_chroma_offset2 by stride vertically and increase by 2 horizontally */
      h_chroma_offset2 = h_chroma_offset2 - stride + 2;
      /* Reduce width by 2 from both the side so effectively it will reduce 4 */
      w = w - 4;
    }

    /* Draw vertical lines */
    for (thickness = 0; thickness < roi_rect_thickness; thickness++) {
      for (i = 0, j = 0; i < (h / vert_sampling); i++) {
        *(v_chroma_offset1 + j) = u;
        *(v_chroma_offset1 + j + 1) = v;
        *(v_chroma_offset3 + j) = u;
        *(v_chroma_offset3 + j + 1) = v;
        j += stride;
      }
      /* Increase v_chroma_offset1 by stride vertically and by 2 horizontally */
      v_chroma_offset1 = v_chroma_offset1 + stride + 2;
      /* Increase v_chroma_offset3 by stride vertically and decrease by 2 horizontally */
      v_chroma_offset3 = v_chroma_offset3 + stride - 2;
      /* Reduce height by stride from both the side so effectively it will reduce by 2
       * for NV16 format and by 4 for NV12 format */
      h = h - (2 * vert_sampling);
    }
  }
  return TRUE;
}

static gboolean
gst_kms_sink_import_dmabuf (GstKMSSink * self, GstBuffer * inbuf,
    GstBuffer ** outbuf)
{
  gint prime_fds[GST_VIDEO_MAX_PLANES] = { 0, };
  GstVideoMeta *meta;
  guint i, n_mem, n_planes;
  GstKMSMemory *kmsmem;
  guint mems_idx[GST_VIDEO_MAX_PLANES];
  gsize mems_skip[GST_VIDEO_MAX_PLANES];
  GstMemory *mems[GST_VIDEO_MAX_PLANES];
  GstMapInfo info;

  if (!self->has_prime_import)
    return FALSE;

  /* This will eliminate most non-dmabuf out there */
  if (!gst_is_dmabuf_memory (gst_buffer_peek_memory (inbuf, 0)))
    return FALSE;

  n_planes = GST_VIDEO_INFO_N_PLANES (&self->vinfo);
  n_mem = gst_buffer_n_memory (inbuf);
  meta = gst_buffer_get_video_meta (inbuf);

  GST_TRACE_OBJECT (self, "Found a dmabuf with %u planes and %u memories",
      n_planes, n_mem);

  /* We cannot have multiple dmabuf per plane */
  if (n_mem > n_planes)
    return FALSE;
  g_assert (n_planes != 0);

  /* Update video info based on video meta */
  if (meta) {
    /* Update YUV444 meta data from original GRAY8/GRAY10 frame */
    if (self->gray_to_yuv444 && (meta->format == GST_VIDEO_FORMAT_GRAY8 ||
            meta->format == GST_VIDEO_FORMAT_GRAY10_LE32)) {
      /* skip the meta info modification in case the original meta height and
       * vinfo height are the same, not sure why the second frame is this case */
      if (meta->height == 3 * GST_VIDEO_INFO_HEIGHT (&self->vinfo)) {
        if (meta->format == GST_VIDEO_FORMAT_GRAY8)
          meta->format == GST_VIDEO_FORMAT_Y444;
        else if (meta->format == GST_VIDEO_FORMAT_GRAY10_LE32)
          meta->format == GST_VIDEO_FORMAT_Y444_10LE32;
        meta->height = GST_VIDEO_INFO_HEIGHT (&self->vinfo);
        meta->n_planes = 3;
        meta->offset[0] = 0;
        /* stride of vinfo for the first frame is not aligned, so recaculate the
         * stride, instead of use stride from vinfo */
        meta->stride[0] = meta->stride[0] + 255 & ~255;
        meta->offset[1] = meta->offset[0] + meta->stride[0] * meta->height;
        meta->stride[1] = meta->stride[0];
        meta->offset[2] = meta->offset[1] + meta->stride[1] * meta->height;
        meta->stride[2] = meta->stride[0];
        GST_DEBUG_OBJECT (self,
            "Meta data modified from GRAY to YUV444, width is %d, height is %d, planes is %d",
            meta->width, meta->height, meta->n_planes);
      }
    }

    GST_VIDEO_INFO_WIDTH (&self->vinfo) = meta->width;
    GST_VIDEO_INFO_HEIGHT (&self->vinfo) = meta->height;

    for (i = 0; i < meta->n_planes; i++) {
      GST_VIDEO_INFO_PLANE_OFFSET (&self->vinfo, i) = meta->offset[i];
      GST_VIDEO_INFO_PLANE_STRIDE (&self->vinfo, i) = meta->stride[i];
    }
  }

  /* Find and validate all memories */
  for (i = 0; i < n_planes; i++) {
    guint length;

    if (!gst_buffer_find_memory (inbuf,
            GST_VIDEO_INFO_PLANE_OFFSET (&self->vinfo, i), 1,
            &mems_idx[i], &length, &mems_skip[i]))
      return FALSE;

    mems[i] = gst_buffer_peek_memory (inbuf, mems_idx[i]);

    /* adjust for memory offset, in case data does not
     * start from byte 0 in the dmabuf fd */
    mems_skip[i] += mems[i]->offset;

    /* And all memory found must be dmabuf */
    if (!gst_is_dmabuf_memory (mems[i]))
      return FALSE;

    if ((i == CHROMA_PLANE) && meta && self->draw_roi) {
      /* Draw ROI feature currently only supported for NV12 & NV16 formats */
      if ((self->vinfo.finfo->format == GST_VIDEO_FORMAT_NV12) ||
          (self->vinfo.finfo->format == GST_VIDEO_FORMAT_NV16)) {
        GST_DEBUG_OBJECT (self, "xlnxkmssink :: Buffer chroma plane received");
        gst_memory_map (mems[i], &info, GST_MAP_READWRITE);
        if (info.data && self->roi_param.coordinate_param
            && self->roi_param.count > 0) {
          draw_rectangle ((info.data + meta->offset[i]),
              self->roi_param.coordinate_param, self->roi_param.count,
              meta->width, meta->height, meta->stride[i],
              self->roi_rect_thickness, &self->roi_rect_yuv_color,
              self->vinfo.finfo->format);
          self->roi_param.count = 0;
          g_free (self->roi_param.coordinate_param);
          self->roi_param.coordinate_param = NULL;
        }
        gst_memory_unmap (mems[i], &info);
      } else {
        GST_DEBUG_OBJECT (self, "Draw ROI feature not supported for %s format",
            gst_video_format_to_string (self->vinfo.finfo->format));
      }
    }
  }
  ensure_kms_allocator (self);

  kmsmem = (GstKMSMemory *) gst_kms_allocator_get_cached (mems[0]);
  if (kmsmem) {
    GST_LOG_OBJECT (self, "found KMS mem %p in DMABuf mem %p", kmsmem, mems[0]);
    goto wrap_mem;
  }

  for (i = 0; i < n_planes; i++)
    prime_fds[i] = gst_dmabuf_memory_get_fd (mems[i]);

  GST_LOG_OBJECT (self, "found these prime ids: %d, %d, %d, %d", prime_fds[0],
      prime_fds[1], prime_fds[2], prime_fds[3]);

  kmsmem = gst_kms_allocator_dmabuf_import (self->allocator,
      prime_fds, n_planes, mems_skip, &self->vinfo);
  if (!kmsmem)
    return FALSE;

  GST_LOG_OBJECT (self, "setting KMS mem %p to DMABuf mem %p with fb id = %d",
      kmsmem, mems[0], kmsmem->fb_id);
  gst_kms_allocator_cache (self->allocator, mems[0], GST_MEMORY_CAST (kmsmem));

wrap_mem:
  *outbuf = gst_buffer_new ();
  if (!*outbuf)
    return FALSE;
  gst_buffer_append_memory (*outbuf, gst_memory_ref (GST_MEMORY_CAST (kmsmem)));
  gst_buffer_add_parent_buffer_meta (*outbuf, inbuf);

  return TRUE;
}

static gboolean
ensure_internal_pool (GstKMSSink * self, GstVideoInfo * in_vinfo,
    GstBuffer * inbuf)
{
  GstBufferPool *pool;
  GstVideoInfo vinfo = *in_vinfo;
  GstVideoMeta *vmeta;
  GstCaps *caps;

  if (self->pool)
    return TRUE;

  /* When cropping, the caps matches the cropped rectangle width/height, but
   * we can retrieve the padded width/height from the VideoMeta (which is kept
   * intact when adding crop meta */
  if ((vmeta = gst_buffer_get_video_meta (inbuf))) {
    vinfo.width = vmeta->width;
    vinfo.height = vmeta->height;
  }

  caps = gst_video_info_to_caps (&vinfo);
  pool = gst_kms_sink_create_pool (self, caps, GST_VIDEO_INFO_SIZE (&vinfo), 2);
  gst_caps_unref (caps);

  if (!pool)
    return FALSE;

  if (!gst_buffer_pool_set_active (pool, TRUE))
    goto activate_pool_failed;

  self->pool = pool;
  return TRUE;

activate_pool_failed:
  {
    GST_ELEMENT_ERROR (self, STREAM, FAILED, ("failed to activate buffer pool"),
        ("failed to activate buffer pool"));
    gst_object_unref (pool);
    return FALSE;
  }

}

static GstBuffer *
gst_kms_sink_copy_to_dumb_buffer (GstKMSSink * self, GstVideoInfo * vinfo,
    GstBuffer * inbuf)
{
  GstFlowReturn ret;
  GstVideoFrame inframe, outframe;
  gboolean success;
  GstBuffer *buf = NULL;

  if (!ensure_internal_pool (self, vinfo, inbuf))
    goto bail;

  ret = gst_buffer_pool_acquire_buffer (self->pool, &buf, NULL);
  if (ret != GST_FLOW_OK)
    goto create_buffer_failed;

  if (self->gray_to_yuv444) {
    GstVideoMeta *meta = gst_buffer_get_video_meta (inbuf);
    meta->format = vinfo->finfo->format;
  }

  if (!gst_video_frame_map (&inframe, vinfo, inbuf, GST_MAP_READ))
    goto error_map_src_buffer;

  if (!gst_video_frame_map (&outframe, vinfo, buf, GST_MAP_WRITE))
    goto error_map_dst_buffer;

  success = gst_video_frame_copy (&outframe, &inframe);
  gst_video_frame_unmap (&outframe);
  gst_video_frame_unmap (&inframe);
  if (!success)
    goto error_copy_buffer;

  return buf;

bail:
  {
    if (buf)
      gst_buffer_unref (buf);
    return NULL;
  }

  /* ERRORS */
create_buffer_failed:
  {
    GST_ELEMENT_ERROR (self, STREAM, FAILED, ("allocation failed"),
        ("failed to create buffer"));
    return NULL;
  }
error_copy_buffer:
  {
    GST_WARNING_OBJECT (self, "failed to upload buffer");
    goto bail;
  }
error_map_dst_buffer:
  {
    gst_video_frame_unmap (&inframe);
    /* fall-through */
  }
error_map_src_buffer:
  {
    GST_WARNING_OBJECT (self, "failed to map buffer");
    goto bail;
  }
}

static GstBuffer *
gst_kms_sink_get_input_buffer (GstKMSSink * self, GstBuffer * inbuf)
{
  GstMemory *mem;
  GstBuffer *buf = NULL;

  mem = gst_buffer_peek_memory (inbuf, 0);
  if (!mem)
    return NULL;

  if (gst_is_kms_memory (mem))
    return gst_buffer_ref (inbuf);

  if (gst_kms_sink_import_dmabuf (self, inbuf, &buf))
    goto done;

  GST_CAT_INFO_OBJECT (CAT_PERFORMANCE, self, "frame copy");
  buf = gst_kms_sink_copy_to_dumb_buffer (self, &self->vinfo, inbuf);

done:
  /* Copy all the non-memory related metas, this way CropMeta will be
   * available upon GstVideoOverlay::expose calls. */
  if (buf)
    gst_buffer_copy_into (buf, inbuf, GST_BUFFER_COPY_METADATA, 0, -1);

  return buf;
}

static void
gst_kms_sink_get_next_vsync_time (GstKMSSink * self, GstClock * clock,
    GstClockTimeDiff * pred_vsync_time)
{
  GstClockTime time;
  GstClockTimeDiff vblank_diff;

  /* Predicted vsync time is when next vsync will come, calculate it from
   * last_vblank time-stamp */
  time = gst_clock_get_time (clock);
  if (GST_CLOCK_TIME_IS_VALID (self->last_vblank)
      && GST_BUFFER_DURATION_IS_VALID (self->last_buffer)) {
    vblank_diff = GST_CLOCK_DIFF (self->last_vblank, time);
    if (vblank_diff < GST_BUFFER_DURATION (self->last_buffer))
      *pred_vsync_time =
          GST_CLOCK_DIFF (vblank_diff, GST_BUFFER_DURATION (self->last_buffer));
    else
      *pred_vsync_time = 0;
  } else {
    *pred_vsync_time = 0;
  }
  GST_DEBUG_OBJECT (self, "got current time: %" GST_TIME_FORMAT
      ", next vsync in %" G_GUINT64_FORMAT, GST_TIME_ARGS (time),
      *pred_vsync_time);
}

static void
xlnx_ll_synchronize (GstKMSSink * self, GstBuffer * buffer, GstClock * clock)
{
  GstReferenceTimestampMeta *meta;
  static GstCaps *caps = NULL;
  GstClockTime time;
  GstClockTimeDiff diff;
  GstClockTimeDiff vblank_diff;
  GstClockTimeDiff pred_vblank_time;
  GstClockTimeDiff wait_time;

  if (G_UNLIKELY (!caps)) {
    caps = gst_caps_new_empty_simple ("timestamp/x-xlnx-ll-decoder-out");
    GST_MINI_OBJECT_FLAG_SET (caps, GST_MINI_OBJECT_FLAG_MAY_BE_LEAKED);
  }

  meta = gst_buffer_get_reference_timestamp_meta (buffer, caps);
  if (!meta) {
    GST_DEBUG_OBJECT (self, "no decoder out meta defined");
    return;
  }

  time = gst_clock_get_time (clock);
  diff = GST_CLOCK_DIFF (meta->timestamp, time);

  gst_kms_sink_get_next_vsync_time (self, clock, &pred_vblank_time);
  wait_time = diff + pred_vblank_time;

  GST_LOG_OBJECT (self,
      "meta: %" GST_TIME_FORMAT " clock: %" GST_TIME_FORMAT
      " diff: %" GST_TIME_FORMAT " frame_time: %" GST_TIME_FORMAT
      " pred_vblank_time: %" GST_TIME_FORMAT, GST_TIME_ARGS (meta->timestamp),
      GST_TIME_ARGS (time), GST_TIME_ARGS (diff),
      GST_TIME_ARGS (GST_BUFFER_DURATION (buffer)),
      GST_TIME_ARGS (pred_vblank_time));

  /* Make sure decoder has enough time (half frame duration) to write the buffer
   * before passing to display */
  if (GST_BUFFER_DURATION_IS_VALID (buffer)
      && wait_time < GST_BUFFER_DURATION (buffer) / 2) {
    /* We didn't wait enough */
    GstClockID clock_id;
    GstClockTimeDiff delta;

    delta = GST_BUFFER_DURATION (buffer) / 2 - wait_time;
    time += delta;

    GST_LOG_OBJECT (self, "need to wait extra %" GST_TIME_FORMAT,
        GST_TIME_ARGS (delta));

    clock_id = gst_clock_new_single_shot_id (clock, time);
    gst_clock_id_wait (clock_id, NULL);
    gst_clock_id_unref (clock_id);
  }
}

static void
gst_kms_sink_fix_field_inversion (GstKMSSink * self, GstBuffer * buffer)
{
  guint32 flags_local = 0;
  GstBuffer *buf = NULL;
  GstMemory *mem;
  guint32 fb_id, old_fb_id;

  old_fb_id = self->buffer_id;
  GST_DEBUG_OBJECT (self,
      "Repeating last buffer and then sending current buffer to achieve resync");

  if (GST_BUFFER_FLAG_IS_SET (buffer, GST_VIDEO_BUFFER_FLAG_ONEFIELD)) {
    buf = self->previous_last_buffer;
    if (GST_BUFFER_FLAG_IS_SET (buffer, GST_VIDEO_BUFFER_FLAG_TFF))
      flags_local |= DRM_MODE_FB_ALTERNATE_BOTTOM;
    else
      flags_local |= DRM_MODE_FB_ALTERNATE_TOP;
  }
  mem = gst_buffer_peek_memory (buf, 0);
  if (!gst_kms_memory_add_fb (mem, &self->vinfo, flags_local)) {
    GST_ERROR_OBJECT (self, "Failed to get buffer object handle");
    return;
  }

  fb_id = gst_kms_memory_get_fb_id (mem);

  if (fb_id == 0) {
    GST_ERROR_OBJECT (self, "Failed to get fb id for previous buffer");
    return;
  }

  self->buffer_id = fb_id;

  if (!gst_kms_sink_sync (self)) {
    GST_ERROR_OBJECT (self,
        "Repeating buffer for correcting field inversion failed");
  } else {
    GST_DEBUG_OBJECT (self,
        "Corrected field inversion by repeating buffer with self->buffer_id = %d, self->crtc_id = %d self->fd %x flags = %x",
        self->buffer_id, self->crtc_id, self->fd, flags_local);
  }
}

static void
gst_kms_sink_avoid_field_inversion (GstKMSSink * self, GstClock * clock)
{
  guint32 flags_local = 0, i = 0;
  GstBuffer *buf = NULL;
  GstClockTimeDiff pred_vsync_time;
  GstMemory *mem;
  guint32 fb_id;

  gst_kms_sink_get_next_vsync_time (self, clock, &pred_vsync_time);
  if (pred_vsync_time != 0 && pred_vsync_time < VSYNC_GAP_USEC * GST_USECOND) {
    for (i = 0; i < 2; i++) {
      if (GST_BUFFER_FLAG_IS_SET (self->previous_last_buffer,
              GST_VIDEO_BUFFER_FLAG_ONEFIELD) && !i) {
        buf = self->previous_last_buffer;
        if (GST_BUFFER_FLAG_IS_SET (buf, GST_VIDEO_BUFFER_FLAG_TFF)) {
          GST_DEBUG_OBJECT (self,
              "Received TOP field, repeating previous last buffer");
          flags_local |= DRM_MODE_FB_ALTERNATE_TOP;
        } else {
          GST_DEBUG_OBJECT (self,
              "Received BOTTOM field, repeating previous last buffer");
          flags_local |= DRM_MODE_FB_ALTERNATE_BOTTOM;
        }
      } else {
        buf = self->last_buffer;
        if (GST_BUFFER_FLAG_IS_SET (buf, GST_VIDEO_BUFFER_FLAG_TFF)) {
          GST_DEBUG_OBJECT (self, "Received TOP field, repeating last buffer");
          flags_local |= DRM_MODE_FB_ALTERNATE_TOP;
        } else {
          GST_DEBUG_OBJECT (self,
              "Received BOTTOM field changing to bottom, repeating last buffer");
          flags_local |= DRM_MODE_FB_ALTERNATE_BOTTOM;
        }
      }
      mem = gst_buffer_peek_memory (buf, 0);
      if (!gst_kms_memory_add_fb (mem, &self->vinfo, flags_local)) {
        GST_DEBUG_OBJECT (self, "Failed to get buffer object for buffer  %d",
            i + 1);
        return;
      }

      fb_id = gst_kms_memory_get_fb_id (mem);
      if (fb_id == 0) {
        GST_DEBUG_OBJECT (self, "Failed to get fb id  for buffer %d", i + 1);
        return;
      }

      self->buffer_id = fb_id;

      GST_DEBUG_OBJECT (self, "displaying repeat fb %d", fb_id);
      GST_DEBUG_OBJECT (self,
          "Repeating buffer %d as vblank was about to miss since pred_vsync was %"
          G_GUINT64_FORMAT, i + 1, pred_vsync_time);
      if (!gst_kms_sink_sync (self)) {
        GST_DEBUG_OBJECT (self, "Repeating buffer failed");
      } else {
        GST_DEBUG_OBJECT (self,
            "Repeated buffer with self->buffer_id = %d, self->crtc_id = %d self->fd %x flags = %x i = %d",
            self->buffer_id, self->crtc_id, self->fd, flags_local, i);
      }
    }
  }
}

static gboolean
handle_sei_info (GstKMSSink * self, GstEvent * event)
{
  const GstStructure *s;
  guint32 payload_type;
  GstBuffer *buf;
  GstMapInfo map;
  s = gst_event_get_structure (event);

  if (!gst_structure_get (s, "payload-type", G_TYPE_UINT, &payload_type,
          "payload", GST_TYPE_BUFFER, &buf, NULL)) {
    GST_WARNING_OBJECT (self, "Failed to parse event");
    return TRUE;
  }
  if (payload_type != 77) {
    GST_WARNING_OBJECT (self,
        "Payload type is not matching to draw boundign box.");
    gst_buffer_unref (buf);
    return TRUE;
  }
  if (!gst_buffer_map (buf, &map, GST_MAP_READ)) {
    GST_WARNING_OBJECT (self, "Failed to map payload buffer");
    gst_buffer_unref (buf);
    return TRUE;
  }

  GST_DEBUG_OBJECT (self, "Requesting  (payload-type=%d)", payload_type);

  gint uint_size = sizeof (unsigned int);
  guint num = 2;
  gint i = 0;
  self->roi_param.ts = *(unsigned int *) (map.data);
  self->roi_param.count = *(unsigned int *) (map.data + (num * uint_size));
  num++;
  self->roi_param.coordinate_param =
      g_malloc0 (self->roi_param.count * sizeof (roi_coordinate));
  GST_DEBUG_OBJECT (self, "xlnxkmssink :: roi count %u\n",
      self->roi_param.count);
  for (i = 0; i < self->roi_param.count; i++) {
    self->roi_param.coordinate_param[i].xmin =
        *(unsigned int *) (map.data + (num * uint_size));
    num++;
    self->roi_param.coordinate_param[i].ymin =
        *(unsigned int *) (map.data + (num * uint_size));
    num++;
    self->roi_param.coordinate_param[i].width =
        *(unsigned int *) (map.data + (num * uint_size));
    num++;
    self->roi_param.coordinate_param[i].height =
        *(unsigned int *) (map.data + (num * uint_size));
    num++;
    GST_DEBUG_OBJECT (self,
        "xlnxkmssink :: frame no, roi no, xmin, ymin, width, height %u::%u::%u::%u::%u\n",
        (i + 1), self->roi_param.coordinate_param[i].xmin,
        self->roi_param.coordinate_param[i].ymin,
        self->roi_param.coordinate_param[i].width,
        self->roi_param.coordinate_param[i].height);
  }
  gst_buffer_unmap (buf, &map);
  gst_buffer_unref (buf);

  return TRUE;
}

static gboolean
gst_kms_sink_sink_event (GstBaseSink * bsink, GstEvent * event)
{
  GstKMSSink *self;
  self = GST_KMS_SINK (bsink);
  if (gst_event_has_name (event, OMX_ALG_GST_EVENT_INSERT_PREFIX_SEI)) {
    GST_DEBUG_OBJECT (self, "xlnxkmssink :: SEI event received\n");
    handle_sei_info (self, event);
  }
  return GST_BASE_SINK_CLASS (parent_class)->event (bsink, event);
}

static GstFlowReturn
gst_kms_sink_show_frame (GstVideoSink * vsink, GstBuffer * buf)
{
  gint ret;
  GstBuffer *buffer = NULL;
  GstMemory *mem;
  guint32 fb_id;
  GstKMSSink *self;
  GstVideoInfo *vinfo = NULL;
  GstVideoCropMeta *crop;
  GstVideoRectangle src = { 0, };
  gint video_width, video_height;
  GstVideoRectangle dst = { 0, };
  GstVideoRectangle result;
  GstFlowReturn res;
  GstClock *clock = NULL;
  guint32 flags = 0;
  gboolean err = FALSE;

  self = GST_KMS_SINK (vsink);

  res = GST_FLOW_ERROR;

  if (buf) {
    buffer = gst_kms_sink_get_input_buffer (self, buf);
    vinfo = &self->vinfo;
    video_width = src.w = GST_VIDEO_SINK_WIDTH (self);
    video_height = src.h = GST_VIDEO_SINK_HEIGHT (self);
  } else if (self->last_buffer) {
    buffer = gst_buffer_ref (self->last_buffer);
    vinfo = &self->last_vinfo;
    video_width = src.w = self->last_width;
    video_height = src.h = self->last_height;
  }

  clock = gst_element_get_clock (GST_ELEMENT_CAST (self));
  if (!clock) {
    GST_DEBUG_OBJECT (self, "no clock set yet");
    goto bail;
  }

  if (self->xlnx_ll)
    xlnx_ll_synchronize (self, buffer, clock);

  if (!buffer)
    return GST_FLOW_ERROR;

  if (GST_VIDEO_INFO_INTERLACE_MODE (&self->last_vinfo)) {
    if (self->last_buffer && self->prev_last_vblank
        && self->avoid_field_inversion)
      gst_kms_sink_avoid_field_inversion (self, clock);

    if (((err = find_property_value_for_plane_id (self->fd,
                    self->primary_plane_id, "fid_err")) == 1)
        && self->previous_last_buffer) {
      GST_WARNING_OBJECT (self,
          "Error bit is set we are in inversion mode as fid_err = %d", err);
      gst_kms_sink_fix_field_inversion (self, buffer);
    }
  }

  if (GST_BUFFER_FLAG_IS_SET (buffer, GST_VIDEO_BUFFER_FLAG_ONEFIELD)) {
    if (GST_BUFFER_FLAG_IS_SET (buffer, GST_VIDEO_BUFFER_FLAG_TFF)) {
      GST_DEBUG_OBJECT (vsink, "Received TOP field.");
      flags |= DRM_MODE_FB_ALTERNATE_TOP;
    } else {
      GST_DEBUG_OBJECT (vsink, "Received BOTTOM field.");
      flags |= DRM_MODE_FB_ALTERNATE_BOTTOM;
    }
  }

  mem = gst_buffer_peek_memory (buffer, 0);
  if (!gst_kms_memory_add_fb (mem, &self->vinfo, flags))
    goto buffer_invalid;

  fb_id = gst_kms_memory_get_fb_id (mem);
  if (fb_id == 0)
    goto buffer_invalid;

  GST_TRACE_OBJECT (self, "displaying fb %d", fb_id);

  GST_OBJECT_LOCK (self);
  if (self->modesetting_enabled) {
    self->buffer_id = fb_id;
    goto sync_frame;
  }

  if ((crop = gst_buffer_get_video_crop_meta (buffer))) {
    GstVideoInfo cropped_vinfo = *vinfo;

    cropped_vinfo.width = crop->width;
    cropped_vinfo.height = crop->height;

    if (!gst_kms_sink_calculate_display_ratio (self, &cropped_vinfo, &src.w,
            &src.h))
      goto no_disp_ratio;

    src.x = crop->x;
    src.y = crop->y;
  }

  dst.w = self->render_rect.w;
  dst.h = self->render_rect.h;

retry_set_plane:
  gst_video_sink_center_rect (src, dst, &result, self->can_scale);

  result.x += self->render_rect.x;
  result.y += self->render_rect.y;

  if (crop) {
    src.w = crop->width;
    src.h = crop->height;
  } else {
    src.w = video_width;
    src.h = video_height;
  }

  /* handle out of screen case */
  if ((result.x + result.w) > self->hdisplay)
    result.w = self->hdisplay - result.x;

  if ((result.y + result.h) > self->vdisplay)
    result.h = self->vdisplay - result.y;

  if (result.w <= 0 || result.h <= 0) {
    GST_WARNING_OBJECT (self, "video is out of display range");
    goto sync_frame;
  }

  /* to make sure it can be show when driver don't support scale */
  if (!self->can_scale) {
    src.w = result.w;
    src.h = result.h;
  }

  GST_TRACE_OBJECT (self,
      "drmModeSetPlane at (%i,%i) %ix%i sourcing at (%i,%i) %ix%i",
      result.x, result.y, result.w, result.h, src.x, src.y, src.w, src.h);

  ret = drmModeSetPlane (self->fd, self->plane_id, self->crtc_id, fb_id, 0,
      result.x, result.y, result.w, result.h,
      /* source/cropping coordinates are given in Q16 */
      src.x << 16, src.y << 16, src.w << 16, src.h << 16);
  if (ret) {
    if (self->can_scale) {
      self->can_scale = FALSE;
      goto retry_set_plane;
    }
    goto set_plane_failed;
  }

sync_frame:
  /* Wait for the previous frame to complete redraw */
  if (!gst_kms_sink_sync (self)) {
    GST_OBJECT_UNLOCK (self);
    goto bail;
  }

  if (clock) {
    if (GST_CLOCK_TIME_IS_VALID (self->last_vblank))
      self->prev_last_vblank = self->last_vblank;
    self->last_vblank = gst_clock_get_time (clock);
    gst_object_unref (clock);
  }

  /* Save the rendered buffer and its metadata in case a redraw is needed */
  if (buffer != self->last_buffer) {
    if (self->hold_extra_sample)
      gst_buffer_replace (&self->previous_last_buffer, self->last_buffer);
    gst_buffer_replace (&self->last_buffer, buffer);
    self->last_width = GST_VIDEO_SINK_WIDTH (self);
    self->last_height = GST_VIDEO_SINK_HEIGHT (self);
    self->last_vinfo = self->vinfo;
  } else {
    if (self->hold_extra_sample) {
      gst_buffer_unref (self->previous_last_buffer);
      self->hold_extra_sample = FALSE;
    } else {
      gst_buffer_unref (self->last_buffer);
    }
  }

  /* For fullscreen_enabled, tmp_kmsmem is used just to set CRTC mode */
  if (self->modesetting_enabled)
    g_clear_pointer (&self->tmp_kmsmem, gst_memory_unref);

  GST_OBJECT_UNLOCK (self);
  res = GST_FLOW_OK;

bail:
  gst_buffer_unref (buffer);
  return res;

  /* ERRORS */
buffer_invalid:
  {
    GST_ERROR_OBJECT (self, "invalid buffer: it doesn't have a fb id");
    goto bail;
  }
set_plane_failed:
  {
    GST_OBJECT_UNLOCK (self);
    GST_DEBUG_OBJECT (self, "result = { %d, %d, %d, %d} / "
        "src = { %d, %d, %d %d } / dst = { %d, %d, %d %d }", result.x, result.y,
        result.w, result.h, src.x, src.y, src.w, src.h, dst.x, dst.y, dst.w,
        dst.h);
    GST_ELEMENT_ERROR (self, RESOURCE, FAILED,
        (NULL), ("drmModeSetPlane failed: %s (%d)", g_strerror (errno), errno));
    goto bail;
  }
no_disp_ratio:
  {
    GST_OBJECT_UNLOCK (self);
    GST_ELEMENT_ERROR (self, CORE, NEGOTIATION, (NULL),
        ("Error calculating the output display ratio of the video."));
    goto bail;
  }
}

static void
gst_kms_sink_drain (GstKMSSink * self)
{
  GstParentBufferMeta *parent_meta;

  if (!self->last_buffer)
    return;

  /* We only need to return the last_buffer if it depends on upstream buffer.
   * In this case, the last_buffer will have a GstParentBufferMeta set. */
  parent_meta = gst_buffer_get_parent_buffer_meta (self->last_buffer);
  if (parent_meta) {
    GstBuffer *dumb_buf, *last_buf;

    /* If this was imported from our dumb buffer pool we can safely skip the
     * drain */
    if (parent_meta->buffer->pool &&
        GST_IS_KMS_BUFFER_POOL (parent_meta->buffer->pool))
      return;

    GST_DEBUG_OBJECT (self, "draining");

    dumb_buf = gst_kms_sink_copy_to_dumb_buffer (self, &self->last_vinfo,
        parent_meta->buffer);
    last_buf = self->last_buffer;
    /* Take an additional ref as 'self->last_buffer' will be unreferenced
     *  twice during the 'stop'. It will be unreferenced explicitely
     *  and then during the pool destruction. */
    self->last_buffer = gst_buffer_ref (dumb_buf);

    gst_kms_allocator_clear_cache (self->allocator);
    gst_kms_sink_show_frame (GST_VIDEO_SINK (self), NULL);
    gst_buffer_unref (last_buf);
  }
}

static gboolean
gst_kms_sink_query (GstBaseSink * bsink, GstQuery * query)
{
  GstKMSSink *self = GST_KMS_SINK (bsink);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_ALLOCATION:
    case GST_QUERY_DRAIN:
    {
      gst_kms_sink_drain (self);
      break;
    }
    default:
      break;
  }

  return GST_BASE_SINK_CLASS (parent_class)->query (bsink, query);
}

static void
gst_kms_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstKMSSink *sink;

  sink = GST_KMS_SINK (object);

  switch (prop_id) {
    case PROP_DRIVER_NAME:
      g_free (sink->devname);
      sink->devname = g_value_dup_string (value);
      break;
    case PROP_BUS_ID:
      g_free (sink->bus_id);
      sink->bus_id = g_value_dup_string (value);
      break;
    case PROP_CONNECTOR_ID:
      sink->conn_id = g_value_get_int (value);
      break;
    case PROP_PLANE_ID:
      sink->plane_id = g_value_get_int (value);
      break;
    case PROP_FORCE_MODESETTING:
      sink->modesetting_enabled = g_value_get_boolean (value);
      break;
    case PROP_RESTORE_CRTC:
      sink->restore_crtc = g_value_get_boolean (value);
      break;
    case PROP_CAN_SCALE:
      sink->can_scale = g_value_get_boolean (value);
      break;
    case PROP_HOLD_EXTRA_SAMPLE:
      sink->hold_extra_sample = g_value_get_boolean (value);
      break;
    case PROP_DO_TIMESTAMP:
      sink->do_timestamp = g_value_get_boolean (value);
      break;
    case PROP_AVOID_FIELD_INVERSION:
      sink->avoid_field_inversion = g_value_get_boolean (value);
      break;
    case PROP_CONNECTOR_PROPS:{
      const GstStructure *s = gst_value_get_structure (value);

      g_clear_pointer (&sink->connector_props, gst_structure_free);

      if (s)
        sink->connector_props = gst_structure_copy (s);

      break;
    }
    case PROP_PLANE_PROPS:{
      const GstStructure *s = gst_value_get_structure (value);

      g_clear_pointer (&sink->plane_props, gst_structure_free);

      if (s)
        sink->plane_props = gst_structure_copy (s);

      break;
    }
    case PROP_FULLSCREEN_OVERLAY:
      sink->fullscreen_enabled = g_value_get_boolean (value);
      break;
    case PROP_FORCE_NTSC_TV:
      sink->force_ntsc_tv = g_value_get_boolean (value);
      break;
    case PROP_GRAY_TO_YUV444:
      sink->gray_to_yuv444 = g_value_get_boolean (value);
      break;
    case PROP_DRAW_ROI:
      sink->draw_roi = g_value_get_boolean (value);
      break;
    case PROP_ROI_RECT_THICKNESS:
      sink->roi_rect_thickness = g_value_get_uint (value);
      break;
    case PROP_ROI_RECT_COLOR:
      if (gst_value_array_get_size (value) == 3)
        g_value_copy (value, &sink->roi_rect_yuv_color);
      else
        GST_DEBUG_OBJECT (sink,
            "Badly formatted color value, must contain three gint");
      break;
    default:
      if (!gst_video_overlay_set_property (object, PROP_N, prop_id, value))
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_kms_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstKMSSink *sink;

  sink = GST_KMS_SINK (object);

  switch (prop_id) {
    case PROP_DRIVER_NAME:
      g_value_set_string (value, sink->devname);
      break;
    case PROP_BUS_ID:
      g_value_set_string (value, sink->bus_id);
      break;
    case PROP_CONNECTOR_ID:
      g_value_set_int (value, sink->conn_id);
      break;
    case PROP_PLANE_ID:
      g_value_set_int (value, sink->plane_id);
      break;
    case PROP_FORCE_MODESETTING:
      g_value_set_boolean (value, sink->modesetting_enabled);
      break;
    case PROP_RESTORE_CRTC:
      g_value_set_boolean (value, sink->restore_crtc);
      break;
    case PROP_CAN_SCALE:
      g_value_set_boolean (value, sink->can_scale);
      break;
    case PROP_DISPLAY_WIDTH:
      GST_OBJECT_LOCK (sink);
      g_value_set_int (value, sink->hdisplay);
      GST_OBJECT_UNLOCK (sink);
      break;
    case PROP_DISPLAY_HEIGHT:
      GST_OBJECT_LOCK (sink);
      g_value_set_int (value, sink->vdisplay);
      GST_OBJECT_UNLOCK (sink);
      break;
    case PROP_HOLD_EXTRA_SAMPLE:
      g_value_set_boolean (value, sink->hold_extra_sample);
      break;
    case PROP_DO_TIMESTAMP:
      g_value_set_boolean (value, sink->do_timestamp);
      break;
    case PROP_AVOID_FIELD_INVERSION:
      g_value_set_boolean (value, sink->avoid_field_inversion);
      break;
    case PROP_CONNECTOR_PROPS:
      gst_value_set_structure (value, sink->connector_props);
      break;
    case PROP_PLANE_PROPS:
      gst_value_set_structure (value, sink->plane_props);
    case PROP_FULLSCREEN_OVERLAY:
      g_value_set_boolean (value, sink->fullscreen_enabled);
      break;
    case PROP_FORCE_NTSC_TV:
      g_value_set_boolean (value, sink->force_ntsc_tv);
      break;
    case PROP_GRAY_TO_YUV444:
      g_value_set_boolean (value, sink->gray_to_yuv444);
      break;
    case PROP_DRAW_ROI:
      g_value_set_boolean (value, sink->draw_roi);
      break;
    case PROP_ROI_RECT_THICKNESS:
      g_value_set_uint (value, sink->roi_rect_thickness);
      break;
    case PROP_ROI_RECT_COLOR:
      g_value_copy (&sink->roi_rect_yuv_color, value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_kms_sink_finalize (GObject * object)
{
  GstKMSSink *sink;

  sink = GST_KMS_SINK (object);
  g_clear_pointer (&sink->devname, g_free);
  g_clear_pointer (&sink->bus_id, g_free);
  gst_poll_free (sink->poll);
  g_clear_pointer (&sink->connector_props, gst_structure_free);
  g_clear_pointer (&sink->plane_props, gst_structure_free);
  g_clear_pointer (&sink->tmp_kmsmem, gst_memory_unref);
  g_value_unset (&sink->roi_rect_yuv_color);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_kms_sink_init (GstKMSSink * sink)
{
  sink->fd = -1;
  sink->conn_id = -1;
  sink->plane_id = -1;
  sink->can_scale = TRUE;
  sink->last_vblank = GST_CLOCK_TIME_NONE;
  sink->prev_last_vblank = GST_CLOCK_TIME_NONE;
  sink->last_ts = GST_CLOCK_TIME_NONE;
  sink->last_orig_ts = GST_CLOCK_TIME_NONE;
  gst_poll_fd_init (&sink->pollfd);
  sink->poll = gst_poll_new (TRUE);
  gst_video_info_init (&sink->vinfo);
  g_value_init (&sink->roi_rect_yuv_color, GST_TYPE_ARRAY);
}

static void
gst_kms_sink_class_init (GstKMSSinkClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;
  GstBaseSinkClass *basesink_class;
  GstVideoSinkClass *videosink_class;
  GstCaps *caps;

  gobject_class = G_OBJECT_CLASS (klass);
  element_class = GST_ELEMENT_CLASS (klass);
  basesink_class = GST_BASE_SINK_CLASS (klass);
  videosink_class = GST_VIDEO_SINK_CLASS (klass);

  gst_element_class_set_static_metadata (element_class, "KMS video sink",
      "Sink/Video", GST_PLUGIN_DESC, "Víctor Jáquez <vjaquez@igalia.com>");

  caps = gst_kms_sink_caps_template_fill ();
  gst_element_class_add_pad_template (element_class,
      gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS, caps));
  gst_caps_unref (caps);

  basesink_class->start = GST_DEBUG_FUNCPTR (gst_kms_sink_start);
  basesink_class->stop = GST_DEBUG_FUNCPTR (gst_kms_sink_stop);
  basesink_class->get_times = GST_DEBUG_FUNCPTR (gst_kms_sink_get_times);
  basesink_class->set_caps = GST_DEBUG_FUNCPTR (gst_kms_sink_set_caps);
  basesink_class->get_caps = GST_DEBUG_FUNCPTR (gst_kms_sink_get_caps);
  basesink_class->propose_allocation = gst_kms_sink_propose_allocation;
  basesink_class->query = gst_kms_sink_query;

  videosink_class->show_frame = gst_kms_sink_show_frame;

  basesink_class->event = GST_DEBUG_FUNCPTR (gst_kms_sink_sink_event);
  gobject_class->finalize = gst_kms_sink_finalize;
  gobject_class->set_property = gst_kms_sink_set_property;
  gobject_class->get_property = gst_kms_sink_get_property;

  /**
   * kmssink:driver-name:
   *
   * If you have a system with multiple GPUs, you can choose which GPU
   * to use setting the DRM device driver name. Otherwise, the first
   * one from an internal list is used.
   */
  g_properties[PROP_DRIVER_NAME] = g_param_spec_string ("driver-name",
      "device name", "DRM device driver name", NULL,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT);

  /**
   * kmssink:bus-id:
   *
   * If you have a system with multiple displays for the same driver-name,
   * you can choose which display to use by setting the DRM bus ID. Otherwise,
   * the driver decides which one.
   */
  g_properties[PROP_BUS_ID] = g_param_spec_string ("bus-id",
      "Bus ID", "DRM bus ID", NULL,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT);

  /**
   * kmssink:connector-id:
   *
   * A GPU has several output connectors, for example: LVDS, VGA,
   * HDMI, etc. By default the first LVDS is tried, then the first
   * eDP, and at the end, the first connected one.
   */
  g_properties[PROP_CONNECTOR_ID] = g_param_spec_int ("connector-id",
      "Connector ID", "DRM connector id", -1, G_MAXINT32, -1,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT);

   /**
   * kmssink:plane-id:
   *
   * There could be several planes associated with a CRTC.
   * By default the first plane that's possible to use with a given
   * CRTC is tried.
   */
  g_properties[PROP_PLANE_ID] = g_param_spec_int ("plane-id",
      "Plane ID", "DRM plane id", -1, G_MAXINT32, -1,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT);

  /**
   * kmssink:force-modesetting:
   *
   * If the output connector is already active, the sink automatically uses an
   * overlay plane. Enforce mode setting in the kms sink and output to the
   * base plane to override the automatic behavior.
   */
  g_properties[PROP_FORCE_MODESETTING] =
      g_param_spec_boolean ("force-modesetting", "Force modesetting",
      "When enabled, the sink try to configure the display mode", FALSE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT);

  /**
   * kmssink:restore-crtc:
   *
   * Restore previous CRTC setting if new CRTC mode was set forcefully.
   * By default this is enabled if user set CRTC with a new mode on an already
   * active CRTC wich was having a valid mode.
   */
  g_properties[PROP_RESTORE_CRTC] =
      g_param_spec_boolean ("restore-crtc", "Restore CRTC mode",
      "When enabled and CRTC was set with a new mode, previous CRTC mode will"
      "be restored when going to NULL state.", TRUE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT);

  /**
   * kmssink:can-scale:
   *
   * User can tell kmssink if the driver can support scale.
   */
  g_properties[PROP_CAN_SCALE] =
      g_param_spec_boolean ("can-scale", "can scale",
      "User can tell kmssink if the driver can support scale", TRUE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT);

  /**
   * kmssink:display-width
   *
   * Actual width of the display. This is read only and only available in
   * PAUSED and PLAYING state. It's meant to be used with
   * gst_video_overlay_set_render_rectangle() function.
   */
  g_properties[PROP_DISPLAY_WIDTH] =
      g_param_spec_int ("display-width", "Display Width",
      "Width of the display surface in pixels", 0, G_MAXINT, 0,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * kmssink:display-height
   *
   * Actual height of the display. This is read only and only available in
   * PAUSED and PLAYING state. It's meant to be used with
   * gst_video_overlay_set_render_rectangle() function.
   */
  g_properties[PROP_DISPLAY_HEIGHT] =
      g_param_spec_int ("display-height", "Display Height",
      "Height of the display surface in pixels", 0, G_MAXINT, 0,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * kmssink: hold-extra-sample:
   *
   * Xilinx Video IP's like mixer, framebuffer read work in
   * asynchronous mode of operation where register settings
   * for next frame can be programmed at any time rather
   * than waiting first for an interrupt. Due to this newly
   * programmed settings don't become active until currently
   * processed frame completes leading to one frame delay
   * between programming and actual writing of data to memory.
   *
   * Set this property for above scenario so that kmssink doesn't
   * release the buffer which is yet to be consumed by Video ip.
   */
  g_properties[PROP_HOLD_EXTRA_SAMPLE] =
      g_param_spec_boolean ("hold-extra-sample",
      "Hold extra sample",
      "When enabled, the sink will keep references to last two buffers", FALSE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT);

  /**
   * kmssink: do-timestamp:
   *
   * If there is no clock recovery hardware present, there is
   * a high possibility of clock drift between the display clock
   * and the one used by upstream. To avoid it or minimize it's impact
   * we change the buffer PTS as per vsync interval when this is enabled.
   *
   * FIXME: This is only supported and tested with non-live usecases.
   *
   */
  g_properties[PROP_DO_TIMESTAMP] =
      g_param_spec_boolean ("do-timestamp",
      "Do timestamp",
      "Do Timestamping as per vsync interval", FALSE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT);

  /**
   * kmssink: avoid-field-inversion:
   *
   * If display clock running faster than rate at which input is coming,
   * at some point it's highly likely that we fail to submit buffer
   * before vsync, and in case of alternate interlace mode it may cause
   * field inversion problem as display will repeat previous field with
   * invert polarity. To avoid this, kmssink predicts the next vblank and
   * if we are too close to vblank, we repeat previous pair before submitting
   * next field.
   */
  g_properties[PROP_AVOID_FIELD_INVERSION] =
      g_param_spec_boolean ("avoid-field-inversion",
      "Avoid field inversion",
      "Predict and avoid field inversion by repeating previous pair", FALSE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT);

  /**
   * kmssink:connector-properties:
   *
   * Additional properties for the connector. Keys are strings and values
   * unsigned 64 bits integers.
   *
   * Since: 1.16
   */
  g_properties[PROP_CONNECTOR_PROPS] =
      g_param_spec_boxed ("connector-properties", "Connector Properties",
      "Additional properties for the connector",
      GST_TYPE_STRUCTURE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  /**
   * kmssink:plane-properties:
   *
   * Additional properties for the plane. Keys are strings and values
   * unsigned 64 bits integers.
   *
   * Since: 1.16
   */
  g_properties[PROP_PLANE_PROPS] =
      g_param_spec_boxed ("plane-properties", "Connector Plane",
      "Additional properties for the plane",
      GST_TYPE_STRUCTURE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

   /** kmssink:fullscreen-overlay:
   *
   * If the output connector is already active, the sink automatically uses an
   * overlay plane. Enforce mode setting in the kms sink and output to the
   * base plane to override the automatic behavior.
   */
  g_properties[PROP_FULLSCREEN_OVERLAY] =
      g_param_spec_boolean ("fullscreen-overlay",
      "Fullscreen mode",
      "When enabled, the sink sets CRTC size same as input video size", FALSE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT);

  /**
   * kmssink: force-ntsc-tv:
   *
   * Convert NTSC DV (720x480i) content to NTSC TV D1 (720x486i) display.
   */
  g_properties[PROP_FORCE_NTSC_TV] =
      g_param_spec_boolean ("force-ntsc-tv",
      "Convert NTSC DV content to NTSC TV D1 display",
      "When enabled, NTSC DV (720x480i) content is displayed at NTSC TV D1 (720x486i) resolution",
      FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT);

  /**
   * kmssink: gray-to-y444:
   *
   * Convert GRAY (grayscale 1920x3240) video to YUV444 (planar 4:4:4 1920x1080) display.
   */
  g_properties[PROP_GRAY_TO_YUV444] =
      g_param_spec_boolean ("gray-to-y444", "gray to yuv444",
      "Convert GRAY (grayscale 1920x3240) video to YUV444 (planar 4:4:4 1920x1080) display",
      FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT);

  /**
   * kmssink: draw-roi:
   *
   * If draw-roi option is enabled then it will draw roi bounding-boxes on frame.
   */
  g_properties[PROP_DRAW_ROI] =
      g_param_spec_boolean ("draw-roi", "draw roi",
      "Enable draw-roi to draw bounding-boxes on frame",
      FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT);

  /**
   * kmssink: roi-rectangle-thickness:
   *
   * If roi rectangle thickness is provided then it will draw roi bounding-boxes
   * with given thickness.
   */
  g_properties[PROP_ROI_RECT_THICKNESS] =
      g_param_spec_uint ("roi-rectangle-thickness", "roi rectangle thickness",
      "ROI rectangle thickness size to draw bounding-boxes on frame",
      ROI_RECT_THICKNESS_MIN, ROI_RECT_THICKNESS_MAX, ROI_RECT_THICKNESS_MIN,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT);

  /**
   * kmssink: roi-rectangle-color:
   *
   * If roi rectangle color is provided then it will draw roi bounding-boxes
   * with given color.
   */
  g_properties[PROP_ROI_RECT_COLOR] =
      gst_param_spec_array ("roi-rectangle-color", "roi rectangle color",
      "ROI rectangle color ('<Y, U, V>') to draw bounding-boxes on frame",
      g_param_spec_int ("color-val", "Color Value",
          "One of Y, U or V value.", ROI_RECT_COLOR_MIN,
          ROI_RECT_COLOR_MAX, ROI_RECT_COLOR_MIN,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT),
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT);

  g_object_class_install_properties (gobject_class, PROP_N, g_properties);

  gst_video_overlay_install_properties (gobject_class, PROP_N);
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  if (!gst_element_register (plugin, GST_PLUGIN_NAME, GST_RANK_SECONDARY,
          GST_TYPE_KMS_SINK))
    return FALSE;

  return TRUE;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, kms,
    GST_PLUGIN_DESC, plugin_init, VERSION, GST_LICENSE, GST_PACKAGE_NAME,
    GST_PACKAGE_ORIGIN)
