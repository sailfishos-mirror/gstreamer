analytics_sources = files( 'gstanalyticsmeta.c',
                              'gstanalyticsclassificationmtd.c',
                              'gstanalyticsobjectdetectionmtd.c',
                              'gstanalyticsobjecttrackingmtd.c',
                              'gstanalyticssegmentationmtd.c',
                              'gstanalyticsbatchmeta.c',
                              'gsttensormeta.c',
                              'gsttensor.c',
                              'gstanalytics_image_util.c')

analytics_headers = files(    'analytics.h',
                              'gstanalyticsmeta.h',
                              'analytics-meta-prelude.h',
                              'gstanalyticsclassificationmtd.h',
                              'gstanalyticsobjectdetectionmtd.h',
                              'gstanalyticsobjecttrackingmtd.h',
                              'gstanalyticssegmentationmtd.h',
                              'gstanalyticsbatchmeta.h',
                              'gsttensormeta.h',
                              'gsttensor.h',
                              'gstanalytics_image_util.h')

install_headers(analytics_headers, subdir : 'gstreamer-1.0/gst/analytics')

doc_sources = []
foreach s: analytics_sources + analytics_headers
  doc_sources += s.full_path()
endforeach

libs_sources += {
  'analytics': pathsep.join(doc_sources)
}

pkg_name = 'gstreamer-analytics-1.0'
gstanalytics = library('gstanalytics-' + api_version,
  analytics_sources,
  c_args : gst_plugins_bad_args + ['-DGST_USE_UNSTABLE_API', '-DBUILDING_GST_ANALYTICS', '-DG_LOG_DOMAIN="GStreamer-Analytics"'],
  include_directories : [configinc, libsinc],
  version : libversion,
  soversion : soversion,
  darwin_versions : osxversion,
  install : true,
  dependencies : [gstbase_dep, gstvideo_dep],
  link_args : ['-lm'])

pkgconfig.generate(gstanalytics,
  libraries : [gst_dep, gstbase_dep, gstvideo_dep, '-lm'],
  variables : pkgconfig_variables,
  subdirs : pkgconfig_subdirs,
  name : pkg_name,
  description : 'GStreamer analytics metadata library',
)
gen_sources = []

library_def = {'lib': gstanalytics}
if build_gir
  gir = {
    'sources' : analytics_sources + analytics_headers,
    'namespace' : 'GstAnalytics',
    'nsversion' : api_version,
    'identifier_prefix' : ['GstAnalytics', 'Gst'],
    'symbol_prefix' : ['gst_analytics', 'gst'],
    'export_packages' : pkg_name,
    'includes' : ['Gst-1.0', 'GstBase-1.0', 'GstVideo-1.0'],
    'install' : true,
    'extra_args' : gir_init_section + ['-DGST_USE_UNSTABLE_API'],
    'dependencies' : [gstbase_dep, gstvideo_dep]
  }
  library_def += {'gir': [gir]}
  if not static_build
    analytics_gir = gnome.generate_gir(gstanalytics, kwargs: gir)
    library_def += {'gir_targets':  library_def.get('gir_targets', []) + [analytics_gir]}
    gen_sources += analytics_gir
  endif
endif
gst_libraries += [[pkg_name, library_def]]



gstanalytics_dep = declare_dependency(link_with : gstanalytics,
  include_directories : [libsinc],
  sources: gen_sources,
  dependencies : [gstbase_dep, gstvideo_dep])

meson.override_dependency(pkg_name, gstanalytics_dep)
