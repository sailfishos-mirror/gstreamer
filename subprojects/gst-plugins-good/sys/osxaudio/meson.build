osxaudio_sources = [
  'gstosxaudioringbuffer.c',
  'gstosxaudioelement.c',
  'gstosxaudiosink.c',
  'gstosxaudiosrc.c',
  'gstosxcoreaudiocommon.c',
  'gstosxcoreaudio.c',
  'gstosxaudio.c',
  'gstatdec.c',
  'gstatenc.c',
]

have_osxaudio = false
osxaudio_option = get_option('osxaudio')
if osxaudio_option.disabled() or not ['darwin', 'ios'].contains(host_system)
  subdir_done()
endif

if host_system == 'darwin'
  # TODO: https://github.com/mesonbuild/meson/issues/3940
  have_osxaudio = cc.has_header('CoreAudio/CoreAudio.h', required: osxaudio_option)
  osxaudio_sources += ['gstosxaudiodeviceprovider.c']
elif host_system == 'ios'
  have_osxaudio = cc.has_header('CoreAudio/CoreAudioTypes.h', required: osxaudio_option)
endif

if have_osxaudio
  modules = ['CoreAudio', 'AudioToolbox']
  if host_system == 'darwin'
    modules += ['AudioUnit', 'CoreServices']
  endif
  osxaudio_dep = dependency('appleframeworks', modules : modules)

  gstosxaudio = library('gstosxaudio',
    osxaudio_sources,
    c_args : gst_plugins_good_args,
    include_directories : [configinc, libsinc],
    dependencies : [gstaudio_dep, gstpbutils_dep, osxaudio_dep],
    install : true,
    install_dir : plugins_install_dir)
  plugins += [gstosxaudio]
endif
