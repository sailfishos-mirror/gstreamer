flac_sources = [
  'gstflac.c',
  'gstflacelement.c',
  'gstflacdec.c',
  'gstflacenc.c',
  'gstflactag.c',
]

flac_headers = [
  'gstflacenc.h',
  'gstflactag.h',
  'gstflacdec.h',
  'gstflacelements.h',
]

doc_sources = []
foreach s: flac_sources + flac_headers
  doc_sources += meson.current_source_dir() / s
endforeach

plugin_sources += {
  'flac': pathsep.join(doc_sources)
}

flac_dep = dependency('flac', version : '>=1.1.4', required : get_option('flac'),
                      allow_fallback: true)

if flac_dep.found()
  gstflac = library('gstflac',
    flac_sources,
    c_args : gst_plugins_good_args + ['-DGST_USE_UNSTABLE_API'],
    link_args : noseh_link_args,
    include_directories : [configinc, libsinc],
    dependencies : [gstbase_dep, gsttag_dep, gstaudio_dep, flac_dep],
    install : true,
    install_dir : plugins_install_dir,
  )
  plugins += [gstflac]
endif
