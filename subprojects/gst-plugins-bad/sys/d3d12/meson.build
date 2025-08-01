d3d12_sources = [
  'gstd3d12av1dec.cpp',
  'gstd3d12basefilter.cpp',
  'gstd3d12compositor.cpp',
  'gstd3d12convert.cpp',
  'gstd3d12decoder.cpp',
  'gstd3d12decodercpbpool.cpp',
  'gstd3d12deinterlace.cpp',
  'gstd3d12yadif.cpp',
  'gstd3d12dpbstorage.cpp',
  'gstd3d12dxgicapture.cpp',
  'gstd3d12encoder.cpp',
  'gstd3d12encoderbufferpool.cpp',
  'gstd3d12h264dec.cpp',
  'gstd3d12h264enc.cpp',
  'gstd3d12h265dec.cpp',
  'gstd3d12ipc.cpp',
  'gstd3d12ipcclient.cpp',
  'gstd3d12ipcserver.cpp',
  'gstd3d12ipcsink.cpp',
  'gstd3d12ipcsrc.cpp',
  'gstd3d12mpeg2dec.cpp',
  'gstd3d12mipmapping.cpp',
  'gstd3d12overlaycompositor.cpp',
  'gstd3d12pluginutils.cpp',
  'gstd3d12remap.cpp',
  'gstd3d12screencapture.cpp',
  'gstd3d12screencapturedevice.cpp',
  'gstd3d12screencapturesrc.cpp',
  'gstd3d12swapchainsink.cpp',
  'gstd3d12testsrc.cpp',
  'gstd3d12videosink.cpp',
  'gstd3d12vp8dec.cpp',
  'gstd3d12vp9dec.cpp',
  'gstd3d12window-swapchain.cpp',
  'gstd3d12window-win32.cpp',
  'gstd3d12window.cpp',
  'gstd3d12fisheyedewarp.cpp',
  'plugin.cpp',
]

d3d12_headers = [
  'gstd3d12upload.h',
  'gstd3d12decoder.h',
  'gstd3d12screencapturedevice.h',
  'gstd3d12h264dec.h',
  'gstd3d12yadif.h',
  'gstd3d12h264enc.h',
  'gstd3d12convert.h',
  'gstd3d12encoderbufferpool.h',
  'gstd3d12download.h',
  'gstd3d12ipcclient.h',
  'gstd3d12mpeg2dec.h',
  'gstd3d12graphicscapture.h',
  'gstd3d12ipcsink.h',
  'gstd3d12memorycopy.h',
  'gstd3d12decodercpbpool.h',
  'gstd3d12compositor.h',
  'gstd3d12window-swapchain-resource.h',
  'gstd3d12mipgen.h',
  'gstd3d12swapchainsink.h',
  'gstd3d12window-swapchain.h',
  'gstd3d12dpbstorage.h',
  'gstd3d12pluginutils.h',
  'gstd3d12remap.h',
  'gstd3d12h265dec.h',
  'gstd3d12screencapturesrc.h',
  'gstd3d12overlaycompositor.h',
  'gstd3d12window.h',
  'gstd3d12av1dec.h',
  'gstd3d12videosink.h',
  'gstd3d12window-win32.h',
  'gstd3d12encoder.h',
  'gstd3d12dxgicapture.h',
  'gstd3d12mipmapping.h',
  'gstd3d12vp9dec.h',
  'gstd3d12ipc.h',
  'gstd3d12deinterlace.h',
  'gstd3d12ipcserver.h',
  'gstd3d12ipcsrc.h',
  'gstd3d12screencapture.h',
  'gstd3d12basefilter.h',
  'gstd3d12testsrc.h',
  'gstd3d12vp8dec.h',
  'gstd3d12fisheyedewarp.h',
]

graphicscapture_sources = [
  'gstd3d12graphicscapture.cpp',
]

memorycopy_sources = [
  'gstd3d12memorycopy.cpp',
]

download_upload_sources = [
  'gstd3d12download.cpp',
  'gstd3d12upload.cpp',
]

doc_sources = []
foreach s: d3d12_sources + graphicscapture_sources + memorycopy_sources + download_upload_sources + d3d12_headers
  doc_sources += meson.current_source_dir() / s
endforeach

plugin_sources += {
  'd3d12': pathsep.join(doc_sources)
}

extra_args = [
  '-DGST_USE_UNSTABLE_API',
  '-DGST_D3D12_USE_DIRECTX_HEADERS',
]

d3d12_cdata = configuration_data()

# Disable this warning error. Otherwise d3dx12.h will break build
if cc.get_argument_syntax() == 'msvc'
  extra_args += cc.get_supported_arguments([
    '/wd4062', # 'identifier' : unreferenced local variable
  ])
else
  extra_args += cc.get_supported_arguments([
    '-Wno-misleading-indentation',
    # MinGW 32bits compiler seems to be complaining about redundant-decls
    # when ComPtr is in use. Let's just disable the warning
    '-Wno-redundant-decls',
    # include/directxmath/DirectXMathMatrix.inl:1161:16: error: variable 'aa' set but not used
    '-Wno-unused-but-set-variable',
    # include/directx/d3dx12_pipeline_state_stream.h:28: error: ignoring '#pragma warning '
    '-Wno-unknown-pragmas',
    # Various warnings in DirectX-Headers headers
    '-Wno-switch',
    # Ignore missing fallthrough in DirectX-Headers headers
    '-Wimplicit-fallthrough=0',
    '-Wno-undef'
  ])
endif

extra_deps = []

d3d12_option = get_option('d3d12')
d3d12_wgc_option = get_option('d3d12-wgc')
if host_system != 'windows' or d3d12_option.disabled()
  subdir_done()
endif

d3d11_lib = cc.find_library('d3d11', required : d3d12_option)
d2d_lib = cc.find_library('d2d1', required: d3d12_option)
dwmapi_lib = cc.find_library('dwmapi', required: d3d12_wgc_option)
dx_headers_dep = dependency('DirectX-Headers',
    version: '>= 1.611',
    allow_fallback: true,
    required: d3d12_option)

if not gstdxva_dep.found() or not gstd3d12_dep.found() or not gstd3dshader_dep.found() \
  or not dx_headers_dep.found() or not d2d_lib.found() or not d3d11_lib.found()
  if d3d12_option.enabled()
    error('The d3d12 was enabled explicitly, but required dependencies were not found.')
  endif
  subdir_done()
endif

have_wgc = false
if not d3d12_wgc_option.disabled()
  have_wgc = cxx.compiles('''
    #include<windows.h>
    #include<winstring.h>
    #include<roapi.h>
    #include<windows.graphics.capture.h>,
    #include<windows.graphics.capture.interop.h>
    #include<windows.graphics.directx.direct3d11.h>
    #include<windows.graphics.directx.direct3d11.interop.h>
    #include<wrl.h>
    using namespace Microsoft::WRL;
    using namespace ABI::Windows::Graphics::Capture;
    ComPtr<IDirect3D11CaptureFramePoolStatics> pool_statics;
    ComPtr<IDirect3D11CaptureFramePool> pool;
    ComPtr<IGraphicsCaptureSession> session;
    ComPtr<IGraphicsCaptureSession2> session2;
    ComPtr<IGraphicsCaptureSession3> session3;
    ''',
    name: 'Windows Graphics Capture support in Windows SDK')

  if d3d12_wgc_option.enabled() and not have_wgc
    error('The Windows Graphics Capture feature is enabled but not supported by Windows SDK')
  endif
endif

have_dx_math = cxx.compiles('''
    #include <windows.h>
    #include <DirectXMath.h>
    using namespace DirectX;
    int main(int argc, char ** argv) {
      XMMATRIX matrix;
      XMFLOAT4X4 dump;
      matrix = XMMatrixIdentity ();
      XMStoreFloat4x4 (&dump, matrix);
      return 0;
    }
    ''',
    name: 'DirectXMath support in Windows SDK')

if not have_dx_math
  directxmath_dep = dependency('DirectXMath', 'directxmath',
      allow_fallback: true,
      required: d3d12_option)
  if not directxmath_dep.found()
    subdir_done()
  endif
  extra_deps += [directxmath_dep]
endif

d3d12_headers = [
  'd3d11.h',
  'd2d1_3.h',
]

have_d3d12_headers = true
foreach h: d3d12_headers
  if not cc.has_header(h)
    have_d3d12_headers = false
  endif
endforeach

if not have_d3d12_headers
  if d3d12_option.enabled()
    error('The d3d12 plugin was enabled explicitly, but required dependencies were not found.')
  endif
  subdir_done()
endif

if have_wgc and dwmapi_lib.found()
  d3d12_cdata.set('HAVE_WGC', true)
  d3d12_sources += graphicscapture_sources
  extra_deps += [dwmapi_lib]
endif

if gstd3d11_dep.found()
  d3d12_cdata.set('HAVE_GST_D3D11', true)
  extra_deps += [gstd3d11_dep]
  d3d12_sources += memorycopy_sources
else
  d3d12_sources += download_upload_sources
endif

configure_file(
  output: 'gstd3d12plugin-config.h',
  configuration: d3d12_cdata,
)

if cc.get_argument_syntax() != 'msvc' and dx_headers_dep.type_name() != 'internal'
  # Required for MinGW because it has old version RPC headers.
  # DirectX-Headers subproject is doing this already,
  # but not exposed via pkg-config. Define this for installed DirectX-Headers
  extra_args += ['-D__REQUIRED_RPCNDR_H_VERSION__=475']
endif

gstd3d12 = library('gstd3d12',
  d3d12_sources,
  c_args : gst_plugins_bad_args + extra_args,
  cpp_args: gst_plugins_bad_args + extra_args,
  include_directories : [configinc],
  dependencies : [gstbase_dep, gstvideo_dep, gstcodecs_dep, gmodule_dep,
                  gstdxva_dep, gstd3dshader_dep, gstd3d12_dep, d3d11_lib,
                  d2d_lib, dx_headers_dep] + extra_deps,
  install : true,
  install_dir : plugins_install_dir,
)
plugins += [gstd3d12]
