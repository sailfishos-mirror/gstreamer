smoothstreaming_sources = [
  'gstmssdemux.c',
  'gstmssmanifest.c',
  'gstmssfragmentparser.c',
  'gstsmoothstreaming-plugin.c',
]

smoothstreaming_headers = [
  'gstmssmanifest.h',
  'gstmssdemux.h',
  'gstmssfragmentparser.h',
]

doc_sources = []
foreach s: smoothstreaming_sources + smoothstreaming_headers
  doc_sources += meson.current_source_dir() / s
endforeach

plugin_sources += {
  'smoothstreaming': pathsep.join(doc_sources)
}

xml28_dep = dependency('libxml-2.0', version: '>= 2.8', required: get_option('smoothstreaming'),
    default_options: {'python': false})

if xml28_dep.found()
  gstmss = library('gstsmoothstreaming',
    smoothstreaming_sources,
    c_args : gst_plugins_bad_args + ['-DGST_USE_UNSTABLE_API'],
    link_args : noseh_link_args,
    include_directories : [configinc, libsinc],
    dependencies : [gst_dep, gstadaptivedemux_dep, gstcodecparsers_dep,
		    gsturidownloader_dep, gstisoff_dep, xml28_dep, gio_dep],
    install : true,
    install_dir : plugins_install_dir,
  )
  plugins += [gstmss]
endif
