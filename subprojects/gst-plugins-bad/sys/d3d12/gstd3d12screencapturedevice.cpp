/* GStreamer
 * Copyright (C) 2024 Seungha Yang <seungha@centricular.com>
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

#include "gstd3d12screencapturedevice.h"
#include "gstd3d12screencapture.h"
#include "gstd3d12pluginutils.h"
#include <gst/video/video.h>
#include <wrl.h>
#include <string.h>
#include <string>
#include <locale>
#include <codecvt>
#include <set>
#include <vector>

/* *INDENT-OFF* */
using namespace Microsoft::WRL;
/* *INDENT-ON* */

#ifndef GST_DISABLE_GST_DEBUG
#define GST_CAT_DEFAULT ensure_debug_category()
static GstDebugCategory *
ensure_debug_category (void)
{
  static GstDebugCategory *cat = nullptr;

  GST_D3D12_CALL_ONCE_BEGIN {
    cat = _gst_debug_category_new ("d3d12screencapture",
        0, "d3d12screencapture");
  } GST_D3D12_CALL_ONCE_END;

  return cat;
}
#endif

static GstStaticCaps template_caps =
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
    (GST_CAPS_FEATURE_MEMORY_D3D12_MEMORY,
        "{ BGRA, RGBA64_LE }")
    ", pixel-aspect-ratio = 1/1, colorimetry = (string) sRGB; "
    GST_VIDEO_CAPS_MAKE ("{ BGRA, RGBA64_LE }") ", pixel-aspect-ratio = 1/1, "
    "colorimetry = (string) sRGB");

enum
{
  PROP_0,
  PROP_MONITOR_HANDLE,
};

struct _GstD3D12ScreenCaptureDevice
{
  GstDevice parent;

  HMONITOR monitor_handle;

  gchar *device_path;
};

static void gst_d3d12_screen_capture_device_finalize (GObject * object);
static void gst_d3d12_screen_capture_device_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);
static void gst_d3d12_screen_capture_device_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static GstElement *gst_d3d12_screen_capture_device_create_element (GstDevice *
    device, const gchar * name);

#define gst_d3d12_screen_capture_device_parent_class device_parent_class
G_DEFINE_TYPE (GstD3D12ScreenCaptureDevice,
    gst_d3d12_screen_capture_device, GST_TYPE_DEVICE);

static void
gst_d3d12_screen_capture_device_class_init (GstD3D12ScreenCaptureDeviceClass *
    klass)
{
  auto object_class = G_OBJECT_CLASS (klass);
  auto dev_class = GST_DEVICE_CLASS (klass);

  object_class->finalize = gst_d3d12_screen_capture_device_finalize;
  object_class->get_property = gst_d3d12_screen_capture_device_get_property;
  object_class->set_property = gst_d3d12_screen_capture_device_set_property;

  g_object_class_install_property (object_class, PROP_MONITOR_HANDLE,
      g_param_spec_uint64 ("monitor-handle", "Monitor Handle",
          "A HMONITOR handle", 0, G_MAXUINT64, 0,
          (GParamFlags) (G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE |
              G_PARAM_CONSTRUCT_ONLY)));

  dev_class->create_element = gst_d3d12_screen_capture_device_create_element;
}

static void
gst_d3d12_screen_capture_device_init (GstD3D12ScreenCaptureDevice * self)
{
}

static void
gst_d3d12_screen_capture_device_finalize (GObject * object)
{
  auto self = GST_D3D12_SCREEN_CAPTURE_DEVICE (object);

  g_free (self->device_path);

  G_OBJECT_CLASS (device_parent_class)->finalize (object);
}

static void
gst_d3d12_screen_capture_device_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  auto self = GST_D3D12_SCREEN_CAPTURE_DEVICE (object);

  switch (prop_id) {
    case PROP_MONITOR_HANDLE:
      g_value_set_uint64 (value, (guint64) self->monitor_handle);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_d3d12_screen_capture_device_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  auto self = GST_D3D12_SCREEN_CAPTURE_DEVICE (object);

  switch (prop_id) {
    case PROP_MONITOR_HANDLE:
      self->monitor_handle = (HMONITOR) g_value_get_uint64 (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstElement *
gst_d3d12_screen_capture_device_create_element (GstDevice * device,
    const gchar * name)
{
  auto self = GST_D3D12_SCREEN_CAPTURE_DEVICE (device);
  auto elem = gst_element_factory_make ("d3d12screencapturesrc", name);

  g_object_set (elem, "monitor-handle", self->monitor_handle, nullptr);

  return elem;
}

static void gst_d3d12_screen_capture_device_provider_update (gpointer provider);
static gpointer
gst_d3d12_screen_capture_device_provider_monitor_thread (gpointer user_data);

/* *INDENT-OFF* */
class MonitorNotificationManager
{
public:
  MonitorNotificationManager (const MonitorNotificationManager &) = delete;
  MonitorNotificationManager& operator= (const MonitorNotificationManager &) = delete;
  static MonitorNotificationManager * Inst()
  {
    static MonitorNotificationManager *inst = nullptr;
    GST_D3D12_CALL_ONCE_BEGIN {
      inst = new MonitorNotificationManager ();
    } GST_D3D12_CALL_ONCE_END;

    return inst;
  }

  void
  Register (GstD3D12ScreenCaptureDeviceProvider* client)
  {
    std::lock_guard<std::mutex> lk(lock_);
    clients_.insert(client);
  }

  void
  Unregister (GstD3D12ScreenCaptureDeviceProvider* client)
  {
    std::lock_guard<std::mutex> lk(lock_);
    clients_.erase(client);
  }

  void OnDisplayChange ()
  {
    std::vector<gpointer> clients;
    {
      std::lock_guard<std::mutex> lk (lock_);
      for (auto it : clients_)
        clients.push_back (gst_object_ref (it));
    }

    for (auto it : clients) {
      gst_d3d12_screen_capture_device_provider_update (it);
      gst_object_unref (it);
    }
  }

private:
  MonitorNotificationManager()
  {
    thread_ = g_thread_new ("GstD3D12ScreenCaptureMonitor",
        (GThreadFunc) gst_d3d12_screen_capture_device_provider_monitor_thread,
        nullptr);
  }

private:
  std::mutex lock_;
  std::set<GstD3D12ScreenCaptureDeviceProvider *> clients_;
  GThread *thread_;
};
/* *INDENT-ON* */

struct _GstD3D12ScreenCaptureDeviceProvider
{
  GstDeviceProvider parent;
};

G_DEFINE_TYPE (GstD3D12ScreenCaptureDeviceProvider,
    gst_d3d12_screen_capture_device_provider, GST_TYPE_DEVICE_PROVIDER);

static void gst_d3d12_screen_capture_device_provider_dispose (GObject * object);
static GList *gst_d3d12_screen_capture_device_provider_probe (GstDeviceProvider
    * provider);
static gboolean
gst_d3d12_screen_capture_device_provider_start (GstDeviceProvider * provider);
static void
gst_d3d12_screen_capture_device_provider_stop (GstDeviceProvider * provider);

static void
    gst_d3d12_screen_capture_device_provider_class_init
    (GstD3D12ScreenCaptureDeviceProviderClass * klass)
{
  auto object_class = G_OBJECT_CLASS (klass);
  auto provider_class = GST_DEVICE_PROVIDER_CLASS (klass);

  object_class->dispose = gst_d3d12_screen_capture_device_provider_dispose;

  provider_class->probe =
      GST_DEBUG_FUNCPTR (gst_d3d12_screen_capture_device_provider_probe);
  provider_class->start =
      GST_DEBUG_FUNCPTR (gst_d3d12_screen_capture_device_provider_start);
  provider_class->stop =
      GST_DEBUG_FUNCPTR (gst_d3d12_screen_capture_device_provider_stop);

  gst_device_provider_class_set_static_metadata (provider_class,
      "Direct3D12 Screen Capture Device Provider",
      "Source/Monitor", "List Direct3D12 screen capture source devices",
      "Seungha Yang <seungha@centricular.com>");
}

static void
    gst_d3d12_screen_capture_device_provider_init
    (GstD3D12ScreenCaptureDeviceProvider * self)
{
}

static void
gst_d3d12_screen_capture_device_provider_dispose (GObject * object)
{
  auto self = GST_D3D12_SCREEN_CAPTURE_DEVICE_PROVIDER (object);

  MonitorNotificationManager::Inst ()->Unregister (self);

  G_OBJECT_CLASS
      (gst_d3d12_screen_capture_device_provider_parent_class)->dispose (object);
}

static gboolean
get_monitor_name (const MONITORINFOEXW * info,
    DISPLAYCONFIG_TARGET_DEVICE_NAME * target)
{
  UINT32 num_path = 0;
  UINT32 num_mode = 0;
  LONG query_ret;
  DISPLAYCONFIG_PATH_INFO *path_infos = nullptr;
  DISPLAYCONFIG_MODE_INFO *mode_infos = nullptr;
  gboolean ret = FALSE;

  memset (target, 0, sizeof (DISPLAYCONFIG_TARGET_DEVICE_NAME));

  query_ret = GetDisplayConfigBufferSizes (QDC_ONLY_ACTIVE_PATHS,
      &num_path, &num_mode);
  if (query_ret != ERROR_SUCCESS || num_path == 0 || num_mode == 0)
    return FALSE;

  path_infos = g_new0 (DISPLAYCONFIG_PATH_INFO, num_path);
  mode_infos = g_new0 (DISPLAYCONFIG_MODE_INFO, num_mode);

  query_ret = QueryDisplayConfig (QDC_ONLY_ACTIVE_PATHS, &num_path,
      path_infos, &num_mode, mode_infos, nullptr);
  if (query_ret != ERROR_SUCCESS)
    goto out;

  for (UINT32 i = 0; i < num_path; i++) {
    DISPLAYCONFIG_PATH_INFO *p = &path_infos[i];
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source;

    memset (&source, 0, sizeof (DISPLAYCONFIG_SOURCE_DEVICE_NAME));

    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof (DISPLAYCONFIG_SOURCE_DEVICE_NAME);
    source.header.adapterId = p->sourceInfo.adapterId;
    source.header.id = p->sourceInfo.id;

    query_ret = DisplayConfigGetDeviceInfo (&source.header);
    if (query_ret != ERROR_SUCCESS)
      continue;

    if (wcscmp (info->szDevice, source.viewGdiDeviceName) != 0)
      continue;

    DISPLAYCONFIG_TARGET_DEVICE_NAME tmp;

    memset (&tmp, 0, sizeof (DISPLAYCONFIG_TARGET_DEVICE_NAME));

    tmp.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    tmp.header.size = sizeof (DISPLAYCONFIG_TARGET_DEVICE_NAME);
    tmp.header.adapterId = p->sourceInfo.adapterId;
    tmp.header.id = p->targetInfo.id;

    query_ret = DisplayConfigGetDeviceInfo (&tmp.header);
    if (query_ret != ERROR_SUCCESS)
      continue;

    memcpy (target, &tmp, sizeof (DISPLAYCONFIG_TARGET_DEVICE_NAME));

    ret = TRUE;
    break;
  }

out:
  g_free (path_infos);
  g_free (mode_infos);

  return ret;
}

/* XXX: please bump MinGW toolchain version,
 * DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY defined in wingdi.h */
typedef enum
{
  OUTPUT_TECH_OTHER = -1,
  OUTPUT_TECH_HD15 = 0,
  OUTPUT_TECH_SVIDEO = 1,
  OUTPUT_TECH_COMPOSITE_VIDEO = 2,
  OUTPUT_TECH_COMPONENT_VIDEO = 3,
  OUTPUT_TECH_DVI = 4,
  OUTPUT_TECH_HDMI = 5,
  OUTPUT_TECH_LVDS = 6,
  OUTPUT_TECH_D_JPN = 8,
  OUTPUT_TECH_SDI = 9,
  OUTPUT_TECH_DISPLAYPORT_EXTERNAL = 10,
  OUTPUT_TECH_DISPLAYPORT_EMBEDDED = 11,
  OUTPUT_TECH_UDI_EXTERNAL = 12,
  OUTPUT_TECH_UDI_EMBEDDED = 13,
  OUTPUT_TECH_SDTVDONGLE = 14,
  OUTPUT_TECH_MIRACAST = 15,
  OUTPUT_TECH_INDIRECT_WIRED = 16,
  OUTPUT_TECH_INDIRECT_VIRTUAL = 17,
  OUTPUT_TECH_INTERNAL = 0x80000000,
  OUTPUT_TECH_FORCE_UINT32 = 0xFFFFFFFF
} GST_OUTPUT_TECHNOLOGY;

static const gchar *
output_tech_to_string (GST_OUTPUT_TECHNOLOGY tech)
{
  switch (tech) {
    case OUTPUT_TECH_HD15:
      return "hd15";
    case OUTPUT_TECH_SVIDEO:
      return "svideo";
    case OUTPUT_TECH_COMPOSITE_VIDEO:
      return "composite-video";
    case OUTPUT_TECH_DVI:
      return "dvi";
    case OUTPUT_TECH_HDMI:
      return "hdmi";
    case OUTPUT_TECH_LVDS:
      return "lvds";
    case OUTPUT_TECH_D_JPN:
      return "d-jpn";
    case OUTPUT_TECH_SDI:
      return "sdi";
    case OUTPUT_TECH_DISPLAYPORT_EXTERNAL:
      return "displayport-external";
    case OUTPUT_TECH_DISPLAYPORT_EMBEDDED:
      return "displayport-internal";
    case OUTPUT_TECH_UDI_EXTERNAL:
      return "udi-external";
    case OUTPUT_TECH_UDI_EMBEDDED:
      return "udi-embedded";
    case OUTPUT_TECH_SDTVDONGLE:
      return "sdtv";
    case OUTPUT_TECH_MIRACAST:
      return "miracast";
    case OUTPUT_TECH_INDIRECT_WIRED:
      return "indirect-wired";
    case OUTPUT_TECH_INDIRECT_VIRTUAL:
      return "indirect-virtual";
    case OUTPUT_TECH_INTERNAL:
      return "internal";
    default:
      break;
  }

  return "unknown";
}

static GstDevice *
create_device (const DXGI_ADAPTER_DESC * adapter_desc,
    const DXGI_OUTPUT_DESC * output_desc,
    const MONITORINFOEXW * minfo, const DEVMODEW * dev_mode,
    const DISPLAYCONFIG_TARGET_DEVICE_NAME * target)
{
  GstCaps *caps;
  gint width, height, left, top, right, bottom;
  GstStructure *props;
  /* *INDENT-OFF* */
  std::wstring_convert < std::codecvt_utf8 < wchar_t >, wchar_t > converter;
  /* *INDENT-ON* */
  std::string device_name;
  std::string display_name;
  std::string device_path;
  std::string device_description;
  const gchar *output_type;
  gboolean primary = FALSE;
  GstDevice *device;

  left = (gint) dev_mode->dmPosition.x;
  top = (gint) dev_mode->dmPosition.y;
  width = dev_mode->dmPelsWidth;
  height = dev_mode->dmPelsHeight;
  right = left + width;
  bottom = top + height;

  caps = gst_static_caps_get (&template_caps);
  caps = gst_caps_make_writable (caps);
  gst_caps_set_simple (caps,
      "width", G_TYPE_INT, width, "height", G_TYPE_INT, height, nullptr);

  device_name = converter.to_bytes (minfo->szDevice);
  display_name = converter.to_bytes (target->monitorFriendlyDeviceName);
  device_path = converter.to_bytes (target->monitorDevicePath);
  device_description = converter.to_bytes (adapter_desc->Description);
  output_type =
      output_tech_to_string ((GST_OUTPUT_TECHNOLOGY) target->outputTechnology);
  if ((minfo->dwFlags & MONITORINFOF_PRIMARY) != 0)
    primary = TRUE;

  props = gst_structure_new ("d3d12screencapturedevice-proplist",
      "device.api", G_TYPE_STRING, "d3d12",
      "device.name", G_TYPE_STRING, GST_STR_NULL (device_name.c_str ()),
      "device.path", G_TYPE_STRING, GST_STR_NULL (device_path.c_str ()),
      "device.primary", G_TYPE_BOOLEAN, primary,
      "device.type", G_TYPE_STRING, output_type,
      "device.hmonitor", G_TYPE_UINT64, (guint64) output_desc->Monitor,
      "device.adapter.luid", G_TYPE_INT64,
      gst_d3d12_luid_to_int64 (&adapter_desc->AdapterLuid),
      "device.adapter.description", G_TYPE_STRING,
      GST_STR_NULL (device_description.c_str ()),
      "desktop.coordinates.left", G_TYPE_INT,
      (gint) output_desc->DesktopCoordinates.left,
      "desktop.coordinates.top", G_TYPE_INT,
      (gint) output_desc->DesktopCoordinates.top,
      "desktop.coordinates.right", G_TYPE_INT,
      (gint) output_desc->DesktopCoordinates.right,
      "desktop.coordinates.bottom", G_TYPE_INT,
      (gint) output_desc->DesktopCoordinates.bottom,
      "display.coordinates.left", G_TYPE_INT, left,
      "display.coordinates.top", G_TYPE_INT, top,
      "display.coordinates.right", G_TYPE_INT, right,
      "display.coordinates.bottom", G_TYPE_INT, bottom, nullptr);

  device = (GstDevice *) g_object_new (GST_TYPE_D3D12_SCREEN_CAPTURE_DEVICE,
      "display-name", display_name.c_str (), "caps", caps, "device-class",
      "Source/Monitor", "properties", props, "monitor-handle",
      (guint64) output_desc->Monitor, nullptr);

  auto screen_dev = GST_D3D12_SCREEN_CAPTURE_DEVICE (device);
  screen_dev->device_path = g_strdup (device_path.c_str ());

  gst_caps_unref (caps);

  return device;
}

static GList *
gst_d3d12_screen_capture_device_provider_probe (GstDeviceProvider * provider)
{
  GList *devices = nullptr;
  ComPtr < IDXGIFactory1 > factory;
  HRESULT hr = S_OK;

  hr = CreateDXGIFactory1 (IID_PPV_ARGS (&factory));
  if (FAILED (hr))
    return nullptr;

  for (UINT adapter_idx = 0;; adapter_idx++) {
    ComPtr < IDXGIAdapter1 > adapter;
    DXGI_ADAPTER_DESC adapter_desc;

    hr = factory->EnumAdapters1 (adapter_idx, &adapter);
    if (FAILED (hr))
      break;

    hr = adapter->GetDesc (&adapter_desc);
    if (FAILED (hr))
      continue;

    for (UINT output_idx = 0;; output_idx++) {
      ComPtr < IDXGIOutput > output;
      ComPtr < IDXGIOutput1 > output1;
      DXGI_OUTPUT_DESC desc;
      MONITORINFOEXW minfo;
      DEVMODEW dev_mode;
      DISPLAYCONFIG_TARGET_DEVICE_NAME target;
      GstDevice *dev;

      hr = adapter->EnumOutputs (output_idx, &output);
      if (FAILED (hr))
        break;

      hr = output.As (&output1);
      if (FAILED (hr))
        continue;

      hr = output->GetDesc (&desc);
      if (FAILED (hr))
        continue;

      minfo.cbSize = sizeof (MONITORINFOEXW);
      if (!GetMonitorInfoW (desc.Monitor, &minfo))
        continue;

      dev_mode.dmSize = sizeof (DEVMODEW);
      dev_mode.dmDriverExtra = sizeof (POINTL);
      dev_mode.dmFields = DM_POSITION;
      if (!EnumDisplaySettingsW (minfo.szDevice,
              ENUM_CURRENT_SETTINGS, &dev_mode)) {
        continue;
      }

      /* Human readable monitor name is not always availabe, if it's empty
       * fill it with generic one */
      if (!get_monitor_name (&minfo, &target) ||
          wcslen (target.monitorFriendlyDeviceName) == 0) {
        wcscpy (target.monitorFriendlyDeviceName, L"Generic PnP Monitor");
      }

      dev = create_device (&adapter_desc, &desc, &minfo, &dev_mode, &target);
      devices = g_list_append (devices, dev);
    }
  }

  return devices;
}

static gboolean
gst_d3d12_screen_capture_device_is_in_list (GList * list, GstDevice * device)
{
  GList *iter;
  GstStructure *s;
  gboolean found = FALSE;

  s = gst_device_get_properties (device);
  g_assert (s);

  for (iter = list; iter; iter = g_list_next (iter)) {
    GstStructure *other_s;

    other_s = gst_device_get_properties (GST_DEVICE (iter->data));
    g_assert (other_s);

    found = gst_structure_is_equal (s, other_s);

    gst_structure_free (other_s);
    if (found)
      break;
  }

  gst_structure_free (s);

  return found;
}

static void
gst_d3d12_screen_capture_device_provider_update (gpointer self)
{
  auto provider = GST_DEVICE_PROVIDER_CAST (self);
  GList *prev_devices = nullptr;
  GList *new_devices = nullptr;
  GList *to_add = nullptr;
  GList *to_remove = nullptr;
  GList *iter, *walk;

  GST_DEBUG_OBJECT (self, "Device updated");

  GST_OBJECT_LOCK (provider);
  prev_devices = g_list_copy_deep (provider->devices,
      (GCopyFunc) gst_object_ref, nullptr);
  GST_OBJECT_UNLOCK (provider);

  new_devices = gst_d3d12_screen_capture_device_provider_probe (provider);

  /* Ownership of GstDevice for gst_device_provider_device_add()
   * and gst_device_provider_device_remove() is a bit complicated.
   * Remove floating reference here for things to be clear */
  for (iter = new_devices; iter; iter = g_list_next (iter))
    gst_object_ref_sink (iter->data);

  /* Check newly added devices */
  for (iter = new_devices; iter; iter = g_list_next (iter)) {
    if (!gst_d3d12_screen_capture_device_is_in_list (prev_devices,
            GST_DEVICE (iter->data))) {
      to_add = g_list_prepend (to_add, gst_object_ref (iter->data));
    }
  }

  /* Check removed device */
  for (iter = prev_devices; iter; iter = g_list_next (iter)) {
    if (!gst_d3d12_screen_capture_device_is_in_list (new_devices,
            GST_DEVICE (iter->data))) {
      to_remove = g_list_prepend (to_remove, gst_object_ref (iter->data));
    }
  }

  iter = to_remove;
  while (iter) {
    auto prev_dev = GST_D3D12_SCREEN_CAPTURE_DEVICE (iter->data);

    if (!prev_dev->device_path) {
      iter = g_list_next (iter);
      continue;
    }

    walk = to_add;
    bool found = false;
    while (walk) {
      auto new_dev = GST_D3D12_SCREEN_CAPTURE_DEVICE (walk->data);

      if (!new_dev->device_path ||
          g_strcmp0 (prev_dev->device_path, new_dev->device_path)) {
        walk = g_list_next (walk);
        continue;
      }

      gst_device_provider_device_changed (provider, GST_DEVICE (new_dev),
          GST_DEVICE (prev_dev));
      gst_object_unref (new_dev);
      to_add = g_list_delete_link (to_add, walk);
      found = true;
      break;
    }

    if (found) {
      gst_object_unref (prev_dev);
      auto next = iter->next;
      to_remove = g_list_delete_link (to_remove, iter);
      iter = next;
    } else {
      iter = g_list_next (iter);
    }
  }

  for (iter = to_remove; iter; iter = g_list_next (iter))
    gst_device_provider_device_remove (provider, GST_DEVICE (iter->data));

  for (iter = to_add; iter; iter = g_list_next (iter))
    gst_device_provider_device_add (provider, GST_DEVICE (iter->data));

  if (prev_devices)
    g_list_free_full (prev_devices, (GDestroyNotify) gst_object_unref);

  if (to_add)
    g_list_free_full (to_add, (GDestroyNotify) gst_object_unref);

  if (to_remove)
    g_list_free_full (to_remove, (GDestroyNotify) gst_object_unref);
}

static gboolean
gst_d3d12_screen_capture_device_provider_start (GstDeviceProvider * provider)
{
  auto self = GST_D3D12_SCREEN_CAPTURE_DEVICE_PROVIDER (provider);

  auto devices = gst_d3d12_screen_capture_device_provider_probe (provider);
  if (devices) {
    GList *iter;
    for (iter = devices; iter; iter = g_list_next (iter))
      gst_device_provider_device_add (provider, GST_DEVICE (iter->data));

    g_list_free (devices);
  }

  MonitorNotificationManager::Inst ()->Register (self);

  return TRUE;
}

static void
gst_d3d12_screen_capture_device_provider_stop (GstDeviceProvider * provider)
{
  auto self = GST_D3D12_SCREEN_CAPTURE_DEVICE_PROVIDER (provider);

  MonitorNotificationManager::Inst ()->Unregister (self);
}

static LRESULT CALLBACK
gst_d3d12_screen_capture_device_provider_wnd_proc (HWND hwnd,
    UINT msg, WPARAM wparam, LPARAM lparam)
{
  if (msg == WM_DISPLAYCHANGE) {
    MonitorNotificationManager::Inst ()->OnDisplayChange ();
    return 0;
  }

  return DefWindowProcW (hwnd, msg, wparam, lparam);
}

static gpointer
gst_d3d12_screen_capture_device_provider_monitor_thread (gpointer user_data)
{
  static const wchar_t HWND_CLASS_NAME[] =
      L"GstD3D12ScreenCaptureDeviceProvider";

  GST_D3D12_CALL_ONCE_BEGIN {
    WNDCLASSW wc = { };
    wc.lpfnWndProc = gst_d3d12_screen_capture_device_provider_wnd_proc;
    wc.hInstance = GetModuleHandle (nullptr);
    wc.lpszClassName = HWND_CLASS_NAME;
    RegisterClassW (&wc);
  } GST_D3D12_CALL_ONCE_END;

  auto hwnd = CreateWindowExW (0, HWND_CLASS_NAME, L"", 0,
      0, 0, 0, 0, nullptr, nullptr, GetModuleHandle (nullptr), nullptr);

  MSG msg;
  while (GetMessage (&msg, nullptr, 0, 0)) {
    TranslateMessage (&msg);
    DispatchMessage (&msg);
  }

  CloseWindow (hwnd);

  return nullptr;
}
