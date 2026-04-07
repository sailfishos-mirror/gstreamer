/* GStreamer AVF device identity tests
 *
 * Copyright (C) 2026 Dominique Leroux
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

#include <gst/check/gstcheck.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/gstdevice.h>
#include <gst/gstdevicemonitor.h>

#include <TargetConditionals.h>

#if TARGET_OS_OSX
#include <gst/gstmacos.h>
#endif

typedef struct
{
  gchar *unique_id;
  gchar *display_name;
  gint device_index;
} AvfDeviceInfo;

static void
free_avf_device_info (gpointer data)
{
  AvfDeviceInfo *info = data;

  if (info == NULL)
    return;

  g_free (info->unique_id);
  g_free (info->display_name);
  g_free (info);
}

static GPtrArray *
collect_avf_devices (GstDeviceMonitor * monitor)
{
  GList *devices, *iter;
  GPtrArray *result;

  if (monitor == NULL)
    return NULL;

  result = g_ptr_array_new_with_free_func (free_avf_device_info);
  devices = gst_device_monitor_get_devices (monitor);
  for (iter = devices; iter != NULL; iter = g_list_next (iter)) {
    GstDevice *device = GST_DEVICE (iter->data);
    GstStructure *props;
    const gchar *unique_id;
    gint device_index = -1;
    AvfDeviceInfo *info;
    const gchar *device_class;

    device_class = gst_device_get_device_class (device);
    if (device_class == NULL
        || !g_str_has_prefix (device_class, "Video/Source"))
      continue;

    props = gst_device_get_properties (device);
    fail_unless (props != NULL, "AVF device is missing properties");

    unique_id = gst_structure_get_string (props, "avf.unique_id");
    if (unique_id == NULL || *unique_id == '\0') {
      g_clear_pointer (&props, gst_structure_free);
      continue;
    }

    g_object_get (device, "device-index", &device_index, NULL);
    fail_unless (device_index >= 0,
        "AVF device %s is missing a valid device-index",
        GST_STR_NULL (gst_device_get_display_name (device)));

    info = g_new0 (AvfDeviceInfo, 1);
    info->unique_id = g_strdup (unique_id);
    info->display_name = gst_device_get_display_name (device);
    info->device_index = device_index;
    g_ptr_array_add (result, info);

    g_clear_pointer (&props, gst_structure_free);
  }

  g_list_free_full (devices, gst_object_unref);

  return result;
}

static GstDeviceMonitor *
create_started_avf_monitor (void)
{
  GstDeviceMonitor *monitor = gst_device_monitor_new ();

  fail_unless (monitor != NULL, "Could not create GstDeviceMonitor");
  gst_device_monitor_add_filter (monitor, "Video/Source", NULL);
  fail_unless (gst_device_monitor_start (monitor),
      "Could not start GstDeviceMonitor");

  return monitor;
}

static void
open_device_by_index_and_check_unique_id (GstElement * pipeline,
    GstElement * src, GstElement * sink, const AvfDeviceInfo * info,
    guint attempt_index)
{
  const guint max_pulled_samples = 1;
  GstBus *bus = gst_element_get_bus (pipeline);
  GstStateChangeReturn sret;
  gchar *opened_unique_id = NULL;
  gchar *opened_device_name = NULL;
  gchar *failure = NULL;

  fail_unless (bus != NULL);
  g_object_set (src, "device-index", info->device_index, NULL);

  sret = gst_element_set_state (pipeline, GST_STATE_PLAYING);
  fail_unless (sret != GST_STATE_CHANGE_FAILURE,
      "Could not set AVF pipeline to PLAYING for %s",
      GST_STR_NULL (info->display_name));

  sret = gst_element_get_state (pipeline, NULL, NULL, 10 * GST_SECOND);
  if (sret == GST_STATE_CHANGE_FAILURE) {
    GstMessage *msg = gst_bus_timed_pop_filtered (bus, 0, GST_MESSAGE_ERROR);
    if (msg == NULL) {
      failure =
          g_strdup_printf
          ("AVF pipeline failed for %s without posting an error",
          GST_STR_NULL (info->display_name));
    } else {
      GError *error = NULL;
      gchar *debug = NULL;
      gst_message_parse_error (msg, &error, &debug);
      failure =
          g_strdup_printf ("AVF pipeline failed on attempt %u for %s: %s (%s)",
          attempt_index, GST_STR_NULL (info->display_name),
          error ? error->message : "unknown error", GST_STR_NULL (debug));
      g_clear_error (&error);
      g_clear_pointer (&debug, g_free);
      g_clear_pointer (&msg, gst_message_unref);
    }
    goto done;
  }

  g_object_get (src,
      "unique-id", &opened_unique_id, "device-name", &opened_device_name, NULL);
  if (g_strcmp0 (opened_unique_id, info->unique_id) != 0) {
    failure =
        g_strdup_printf
        ("AVF source opened wrong device on attempt %u for %s: device-index %d resolved to unique-id %s (%s) instead of %s (%s)",
        attempt_index, GST_STR_NULL (info->display_name), info->device_index,
        GST_STR_NULL (opened_unique_id), GST_STR_NULL (opened_device_name),
        GST_STR_NULL (info->unique_id), GST_STR_NULL (info->display_name));
    goto done;
  }

  if (g_strcmp0 (opened_device_name, info->display_name) != 0) {
    failure =
        g_strdup_printf
        ("AVF source opened unstable device-name on attempt %u for unique-id %s: got %s instead of %s",
        attempt_index, GST_STR_NULL (info->unique_id),
        GST_STR_NULL (opened_device_name), GST_STR_NULL (info->display_name));
    goto done;
  }

  for (guint i = 0; i < max_pulled_samples; i++) {
    GstSample *sample =
        gst_app_sink_try_pull_sample (GST_APP_SINK (sink), GST_SECOND);
    if (sample == NULL)
      break;

    gst_sample_unref (sample);
  }

done:
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (bus);
  g_clear_pointer (&opened_unique_id, g_free);
  g_clear_pointer (&opened_device_name, g_free);

  if (failure != NULL) {
    gchar *message = failure;
    failure = NULL;
    ck_abort_msg ("%s", message);
    g_free (message);
  }
}

static void
open_device_by_unique_id_and_check_unique_id (GstElement * pipeline,
    GstElement * src, GstElement * sink, const AvfDeviceInfo * info,
    guint attempt_index)
{
  const guint max_pulled_samples = 1;
  GstBus *bus = gst_element_get_bus (pipeline);
  GstStateChangeReturn sret;
  gchar *opened_unique_id = NULL;
  gchar *opened_device_name = NULL;
  gchar *failure = NULL;

  fail_unless (bus != NULL);
  g_object_set (src, "unique-id", info->unique_id, NULL);

  sret = gst_element_set_state (pipeline, GST_STATE_PLAYING);
  fail_unless (sret != GST_STATE_CHANGE_FAILURE,
      "Could not set AVF pipeline to PLAYING for %s",
      GST_STR_NULL (info->display_name));

  sret = gst_element_get_state (pipeline, NULL, NULL, 10 * GST_SECOND);
  if (sret == GST_STATE_CHANGE_FAILURE) {
    GstMessage *msg = gst_bus_timed_pop_filtered (bus, 0, GST_MESSAGE_ERROR);
    if (msg == NULL) {
      failure =
          g_strdup_printf
          ("AVF pipeline failed for %s without posting an error",
          GST_STR_NULL (info->display_name));
    } else {
      GError *error = NULL;
      gchar *debug = NULL;
      gst_message_parse_error (msg, &error, &debug);
      failure =
          g_strdup_printf ("AVF pipeline failed on attempt %u for %s: %s (%s)",
          attempt_index, GST_STR_NULL (info->display_name),
          error ? error->message : "unknown error", GST_STR_NULL (debug));
      g_clear_error (&error);
      g_clear_pointer (&debug, g_free);
      g_clear_pointer (&msg, gst_message_unref);
    }
    goto done;
  }

  g_object_get (src,
      "unique-id", &opened_unique_id, "device-name", &opened_device_name, NULL);
  if (g_strcmp0 (opened_unique_id, info->unique_id) != 0) {
    failure =
        g_strdup_printf
        ("AVF source opened wrong device on attempt %u for %s: requested unique-id %s resolved to unique-id %s (%s)",
        attempt_index, GST_STR_NULL (info->display_name),
        GST_STR_NULL (info->unique_id), GST_STR_NULL (opened_unique_id),
        GST_STR_NULL (opened_device_name));
    goto done;
  }

  if (g_strcmp0 (opened_device_name, info->display_name) != 0) {
    failure =
        g_strdup_printf
        ("AVF source opened unstable device-name on attempt %u for unique-id %s: got %s instead of %s",
        attempt_index, GST_STR_NULL (info->unique_id),
        GST_STR_NULL (opened_device_name), GST_STR_NULL (info->display_name));
    goto done;
  }

  for (guint i = 0; i < max_pulled_samples; i++) {
    GstSample *sample =
        gst_app_sink_try_pull_sample (GST_APP_SINK (sink), GST_SECOND);
    if (sample == NULL)
      break;

    gst_sample_unref (sample);
  }

done:
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (bus);
  g_clear_pointer (&opened_unique_id, g_free);
  g_clear_pointer (&opened_device_name, g_free);

  if (failure != NULL) {
    gchar *message = failure;
    failure = NULL;
    ck_abort_msg ("%s", message);
    g_free (message);
  }
}

GST_START_TEST (test_avf_device_index_stability)
{
  const guint switch_iterations = 3;
  GstDeviceMonitor *monitor = create_started_avf_monitor ();
  GPtrArray *initial_devices = collect_avf_devices (monitor);
  AvfDeviceInfo *device_a;
  AvfDeviceInfo *device_b;
  GstElement *pipeline, *src, *sink, *capsfilter;
  GstCaps *caps;

  if (initial_devices->len < 2) {
    GST_WARNING
        ("Skipping AVF device-index stability test: need at least two AVF devices, found %u",
        initial_devices->len);
    g_ptr_array_unref (initial_devices);
    gst_device_monitor_stop (monitor);
    gst_object_unref (monitor);
    return;
  }

  gst_device_monitor_stop (monitor);
  g_clear_pointer (&monitor, gst_object_unref);

  device_a = g_ptr_array_index (initial_devices, 0);
  device_b = g_ptr_array_index (initial_devices, 1);

  GST_INFO
      ("Opening AVF devices with %u alternating switch iteration(s): %s [%s], %s [%s]",
      switch_iterations, GST_STR_NULL (device_a->display_name),
      device_a->unique_id, GST_STR_NULL (device_b->display_name),
      device_b->unique_id);

  src = gst_element_factory_make ("avfvideosrc", NULL);
  fail_unless (src != NULL, "Could not create avfvideosrc");
  fail_unless (g_object_class_find_property (G_OBJECT_GET_CLASS (src),
          "unique-id") != NULL, "AVF source does not expose unique-id");

  sink = gst_element_factory_make ("appsink", NULL);
  fail_unless (sink != NULL, "Could not create appsink");
  g_object_set (sink,
      "emit-signals", FALSE,
      "async", FALSE, "sync", FALSE, "max-buffers", 8u, "drop", TRUE, NULL);
  capsfilter = gst_element_factory_make ("capsfilter", NULL);
  fail_unless (capsfilter != NULL, "Could not create capsfilter");
  caps = gst_caps_from_string ("video/x-raw,format=BGRA");
  fail_unless (caps != NULL, "Could not build BGRA caps");
  g_object_set (capsfilter, "caps", caps, NULL);
  gst_caps_unref (caps);

  pipeline = gst_pipeline_new ("avfdeviceidentity-device-index");
  fail_unless (pipeline != NULL);
  gst_bin_add_many (GST_BIN (pipeline), src, capsfilter, sink, NULL);
  fail_unless (gst_element_link_many (src, capsfilter, sink, NULL));

  for (guint iteration = 0; iteration < switch_iterations; iteration++) {
    const AvfDeviceInfo *expected =
        ((iteration % 2) == 0) ? device_a : device_b;
    open_device_by_index_and_check_unique_id (pipeline, src, sink, expected,
        iteration + 1);
  }

  gst_object_unref (pipeline);
  g_ptr_array_unref (initial_devices);
}

GST_END_TEST;

GST_START_TEST (test_avf_unique_id_stability)
{
  const guint switch_iterations = 3;
  GstDeviceMonitor *monitor = create_started_avf_monitor ();
  GPtrArray *initial_devices = collect_avf_devices (monitor);
  AvfDeviceInfo *device_a;
  AvfDeviceInfo *device_b;
  GstElement *pipeline, *src, *sink, *capsfilter;
  GstCaps *caps;

  if (initial_devices->len < 1) {
    GST_WARNING
        ("Skipping AVF unique-id stability test: need at least one AVF device, found %u",
        initial_devices->len);
    g_ptr_array_unref (initial_devices);
    gst_device_monitor_stop (monitor);
    gst_object_unref (monitor);
    return;
  }

  gst_device_monitor_stop (monitor);
  g_clear_pointer (&monitor, gst_object_unref);

  device_a = g_ptr_array_index (initial_devices, 0);
  device_b = (initial_devices->len >= 2) ?
      g_ptr_array_index (initial_devices, 1) : device_a;

  if (device_a == device_b) {
    GST_INFO
        ("Opening single AVF device by unique-id with %u successive open iteration(s): %s [%s]",
        switch_iterations, GST_STR_NULL (device_a->display_name),
        device_a->unique_id);
  } else {
    GST_INFO
        ("Opening AVF devices by unique-id with %u alternating switch iteration(s): %s [%s], %s [%s]",
        switch_iterations, GST_STR_NULL (device_a->display_name),
        device_a->unique_id, GST_STR_NULL (device_b->display_name),
        device_b->unique_id);
  }

  src = gst_element_factory_make ("avfvideosrc", NULL);
  fail_unless (src != NULL, "Could not create avfvideosrc");
  fail_unless (g_object_class_find_property (G_OBJECT_GET_CLASS (src),
          "unique-id") != NULL, "AVF source does not expose unique-id");

  sink = gst_element_factory_make ("appsink", NULL);
  fail_unless (sink != NULL, "Could not create appsink");
  g_object_set (sink,
      "emit-signals", FALSE,
      "async", FALSE, "sync", FALSE, "max-buffers", 8u, "drop", TRUE, NULL);
  capsfilter = gst_element_factory_make ("capsfilter", NULL);
  fail_unless (capsfilter != NULL, "Could not create capsfilter");
  caps = gst_caps_from_string ("video/x-raw,format=BGRA");
  fail_unless (caps != NULL, "Could not build BGRA caps");
  g_object_set (capsfilter, "caps", caps, NULL);
  gst_caps_unref (caps);

  pipeline = gst_pipeline_new ("avfdeviceidentity-unique-id");
  fail_unless (pipeline != NULL);
  gst_bin_add_many (GST_BIN (pipeline), src, capsfilter, sink, NULL);
  fail_unless (gst_element_link_many (src, capsfilter, sink, NULL));

  for (guint iteration = 0; iteration < switch_iterations; iteration++) {
    const AvfDeviceInfo *expected =
        ((iteration % 2) == 0) ? device_a : device_b;
    open_device_by_unique_id_and_check_unique_id (pipeline, src, sink,
        expected, iteration + 1);
  }

  gst_object_unref (pipeline);
  g_ptr_array_unref (initial_devices);
}

GST_END_TEST;

static Suite *
avfdeviceidentity_suite (void)
{
  Suite *s = suite_create ("avfdeviceidentity");
  TCase *tc_basic = tcase_create ("device_index");
  const gchar *enable_device_index_stress =
      g_getenv ("GST_AVF_ENABLE_DEVICE_INDEX_STRESS");

  suite_add_tcase (s, tc_basic);
  tcase_add_test (tc_basic, test_avf_unique_id_stability);
  if (g_strcmp0 (enable_device_index_stress, "1") == 0) {
    tcase_add_test (tc_basic, test_avf_device_index_stability);
  } else {
    GST_INFO
        ("Not registering AVF device-index instability test; set GST_AVF_ENABLE_DEVICE_INDEX_STRESS=1 to enable it");
  }

  return s;
}

static int
run_tests (int argc, char **argv, gpointer user_data)
{
  Suite *s;
  SRunner *sr;
  int nfails;

  (void) argc;
  (void) argv;
  (void) user_data;

  s = avfdeviceidentity_suite ();
  sr = srunner_create (s);
  srunner_set_fork_status (sr, CK_NOFORK);
  srunner_run (sr, NULL, NULL, CK_NORMAL);
  nfails = srunner_ntests_failed (sr);
  srunner_free (sr);

  return nfails;
}

int
main (int argc, char **argv)
{
  gst_check_init (&argc, &argv);
#if TARGET_OS_OSX
  return gst_macos_main ((GstMainFunc) run_tests, argc, argv, NULL);
#else
  return run_tests (argc, argv, NULL);
#endif
}
