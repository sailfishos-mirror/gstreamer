va_sources = [
  'plugin.c',
  'gstjpegdecoder.c',
  'gstvaav1dec.c',
  'gstvabase.c',
  'gstvabasedec.c',
  'gstvabaseenc.c',
  'gstvabasetransform.c',
  'gstvacaps.c',
  'gstvacodecalphadecodebin.c',
  'gstvacompositor.c',
  'gstvadecoder.c',
  'gstvadeinterlace.c',
  'gstvadisplay_priv.c',
  'gstvaencoder.c',
  'gstvafilter.c',
  'gstvah264dec.c',
  'gstvah264enc.c',
  'gstvah265dec.c',
  'gstvah265enc.c',
  'gstvajpegdec.c',
  'gstvajpegenc.c',
  'gstvampeg2dec.c',
  'gstvapluginutils.c',
  'gstvaprofile.c',
  'gstvavp8dec.c',
  'gstvavp8enc.c',
  'gstvavp9dec.c',
  'gstvavp9enc.c',
  'gstvavpp.c',
]

va_headers = [
  'gstjpegdecoder.h',
  'gstvaav1dec.h',
  'gstvaav1enc.h',
  'gstvabasedec.h',
  'gstvabaseenc.h',
  'gstvabase.h',
  'gstvabasetransform.h',
  'gstvacaps.h',
  'gstvacompositor.h',
  'gstvadecoder.h',
  'gstvadeinterlace.h',
  'gstvadevice.h',
  'gstvadisplay_priv.h',
  'gstvaencoder.h',
  'gstvafilter.h',
  'gstvah264dec.h',
  'gstvah264enc.h',
  'gstvah265dec.h',
  'gstvah265enc.h',
  'gstvajpegdec.h',
  'gstvajpegenc.h',
  'gstvampeg2dec.h',
  'gstvapluginutils.h',
  'gstvaprofile.h',
  'gstvavp8dec.h',
  'gstvavp8enc.h',
  'gstvavp9dec.h',
  'gstvavp9enc.h',
  'gstvavpp.h',
  'vacompat.h',
  'gstvacodecalphadecodebin.h'
]

va_linux_sources = [
  'gstvadevice_linux.c'
]

va_win32_sources = [
  'gstvadevice_win32.cpp'
]

va_av1enc_sources = [
  'gstvaav1enc.c'
]

va_h266dec_sources = [
  'gstvah266dec.c'
]

doc_sources = []
foreach s: va_sources + va_linux_sources + va_win32_sources + va_av1enc_sources + va_headers
  doc_sources += meson.current_source_dir() / s
endforeach

plugin_sources += {
  'va': pathsep.join(doc_sources)
}

if host_system not in ['linux', 'windows']
  subdir_done()
endif

va_option = get_option('va').require(gstva_dep.found(), error_message: 'va plugin requires libgstva.')
if va_option.disabled()
  subdir_done()
endif

driverdir = libva_dep.get_variable('driverdir', default_value: '')
if driverdir == ''
  driverdir = join_paths(get_option('prefix'), get_option('libdir'), 'dri')
endif

extra_args = [
  '-DLIBVA_DRIVERS_PATH="' + driverdir + '"',
  '-DGST_USE_UNSTABLE_API',
]
extra_dep = []
c_std_arg = ['c_std=c99']

if host_system == 'linux'
  libgudev_dep = dependency('gudev-1.0', required: get_option('udev'), allow_fallback: true)
  if libgudev_dep.found()
    extra_args += ['-DHAVE_GUDEV']
    extra_dep += [libgudev_dep]
  endif
  va_sources += va_linux_sources
else
  va_sources += va_win32_sources
endif

# MSVC does not understand c99
if cc.get_id() == 'msvc'
  c_std_arg = ['c_std=c11']
endif

if libva_dep.version().version_compare('>= 1.15')
  va_sources += va_av1enc_sources
endif

if libva_dep.version().version_compare('>= 1.22')
  va_sources += va_h266dec_sources
endif

gstva = library('gstva',
  va_sources,
  c_args : gst_plugins_bad_args + extra_args,
  cpp_args : gst_plugins_bad_args + extra_args,
  include_directories : [configinc, libsinc],
  dependencies : [gstcodecs_dep, gstva_dep, libgudev_dep, libm, gstpbutils_dep] + extra_dep,
  override_options : c_std_arg,
  install : true,
  install_dir : plugins_install_dir,
)
plugins += [gstva]
