dash_sources = [
  'gstdashdemux.c',
  'gstmpdnode.c',
  'gstmpdrootnode.c',
  'gstmpdbaseurlnode.c',
  'gstmpdutctimingnode.c',
  'gstmpdmetricsnode.c',
  'gstmpdmetricsrangenode.c',
  'gstmpdsnode.c',
  'gstmpdsegmenttimelinenode.c',
  'gstmpdsegmenttemplatenode.c',
  'gstmpdsegmenturlnode.c',
  'gstmpdsegmentlistnode.c',
  'gstmpdsegmentbasenode.c',
  'gstmpdperiodnode.c',
  'gstmpdrepresentationbasenode.c',
  'gstmpdmultsegmentbasenode.c',
  'gstmpdrepresentationnode.c',
  'gstmpdsubrepresentationnode.c',
  'gstmpdcontentcomponentnode.c',
  'gstmpdadaptationsetnode.c',
  'gstmpdsubsetnode.c',
  'gstmpdprograminformationnode.c',
  'gstmpdlocationnode.c',
  'gstmpdreportingnode.c',
  'gstmpdurltypenode.c',
  'gstmpddescriptortypenode.c',
  'gstxmlhelper.c',
  'gstmpdhelper.c',
  'gstmpdparser.c',
  'gstmpdclient.c',
  'gstplugin.c',
  'gstdashsink.c',
]

dash_headers = [
  'gstdashsink.h',
  'gstmpdsegmenttemplatenode.h',
  'gstdashdemux.h',
  'gstmpdsubsetnode.h',
  'gstmpdhelper.h',
  'gstmpdrootnode.h',
  'gstmpdrepresentationnode.h',
  'gstmpdmultsegmentbasenode.h',
  'gstmpdsubrepresentationnode.h',
  'gstmpdsegmentlistnode.h',
  'gstmpdclient.h',
  'gstxmlhelper.h',
  'gstmpdmetricsrangenode.h',
  'gstmpdnode.h',
  'gstmpdlocationnode.h',
  'gstmpdutctimingnode.h',
  'gstmpdsegmenturlnode.h',
  'gstmpdrepresentationbasenode.h',
  'gstmpdsnode.h',
  'gstmpdurltypenode.h',
  'gstmpdprograminformationnode.h',
  'gstmpdsegmenttimelinenode.h',
  'gstmpdsegmentbasenode.h',
  'gstmpdadaptationsetnode.h',
  'gstmpdmetricsnode.h',
  'gstmpdreportingnode.h',
  'gstmpdcontentcomponentnode.h',
  'gstmpddescriptortypenode.h',
  'gstmpdparser.h',
  'gstmpdbaseurlnode.h',
  'gstdash_debug.h',
  'gstmpdperiodnode.h',
]

doc_sources = []
foreach s: dash_sources + dash_headers
  doc_sources += meson.current_source_dir() / s
endforeach

plugin_sources += {
  'dash': pathsep.join(doc_sources)
}

xml2_dep = dependency('libxml-2.0',
  version : '>= 2.8',
  required : get_option('dash'),
  default_options: {'python': false},
)

if xml2_dep.found()
  gstdashdemux = library('gstdash',
    dash_sources,
    c_args : gst_plugins_bad_args + ['-DGST_USE_UNSTABLE_API'],
    link_args : noseh_link_args,
    include_directories : [configinc, libsinc],
    dependencies : [gstadaptivedemux_dep, gsturidownloader_dep, gsttag_dep,
                    gstnet_dep, gstpbutils_dep, gstbase_dep, gstisoff_dep,
                    gio_dep, xml2_dep],
    install : true,
    install_dir : plugins_install_dir,
  )
  plugins += [gstdashdemux]
endif
