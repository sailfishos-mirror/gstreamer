gstinterleave = library('gstinterleave',
  'plugin.c', 'interleave.c', 'deinterleave.c',
  c_args : gst_plugins_good_args,
  include_directories : [configinc, libsinc],
  dependencies : [gstbase_dep, gstaudio_dep],
  install : true,
  install_dir : plugins_install_dir,
)
plugins += [gstinterleave]
