project('gstreamer', 'c',
  version : '1.27.1.1',
  meson_version : '>= 1.4',
  default_options : [ 'warning_level=1',
                      'buildtype=debugoptimized' ])

gst_version = meson.project_version()
version_arr = gst_version.split('.')
gst_version_major = version_arr[0].to_int()
gst_version_minor = version_arr[1].to_int()
gst_version_micro = version_arr[2].to_int()
if version_arr.length() == 4
  gst_version_nano = version_arr[3].to_int()
else
  gst_version_nano = 0
endif
gst_version_is_dev = gst_version_minor % 2 == 1 and gst_version_micro < 90

api_version = '1.0'
soversion = 0
# maintaining compatibility with the previous libtool versioning
# current = minor * 100 + micro
curversion = gst_version_minor * 100 + gst_version_micro
libversion = '@0@.@1@.0'.format(soversion, curversion)
osxversion = curversion + 1

prefix = get_option('prefix')

datadir = join_paths(prefix, get_option('datadir'))
libexecdir = get_option('libexecdir')
helpers_install_dir = join_paths(libexecdir, 'gstreamer-1.0')

cc = meson.get_compiler('c')
host_system = host_machine.system()
static_build = get_option('default_library') == 'static'

if host_system == 'darwin'
  ios_test_code = '''#include <TargetConditionals.h>
  #if ! TARGET_OS_IPHONE
  #error "Not iOS/tvOS/watchOS/iPhoneSimulator"
  #endif'''
  if cc.compiles(ios_test_code, name : 'building for iOS')
    host_system = 'ios'
  endif

  add_languages('objc', native: false, required: true)
endif

cdata = configuration_data()

if cc.get_id() == 'msvc'
  msvc_args = [
      # Ignore several spurious warnings for things gstreamer does very commonly
      # If a warning is completely useless and spammy, use '/wdXXXX' to suppress it
      # If a warning is harmless but hard to fix, use '/woXXXX' so it's shown once
      # NOTE: Only add warnings here if you are sure they're spurious
      '/wd4018', # implicit signed/unsigned conversion
      '/wd4146', # unary minus on unsigned (beware INT_MIN)
      '/wd4244', # lossy type conversion (e.g. double -> int)
      '/wd4305', # truncating type conversion (e.g. double -> float)
      cc.get_supported_arguments(['/utf-8']), # set the input encoding to utf-8
  ]

  if gst_version_is_dev
    # Enable some warnings on MSVC to match GCC/Clang behaviour
    msvc_args += cc.get_supported_arguments([
      '/we4002', # too many actual parameters for macro 'identifier'
      '/we4003', # not enough actual parameters for macro 'identifier'
      '/we4013', # 'function' undefined; assuming extern returning int
      '/we4020', # 'function' : too many actual parameters
      '/we4027', # function declared without formal parameter list
      '/we4029', # declared formal parameter list different from definition
      '/we4033', # 'function' must return a value
      '/we4045', # 'array' : array bounds overflow
      '/we4047', # 'operator' : 'identifier1' differs in levels of indirection from 'identifier2'
      '/we4053', # one void operand for '?:'
      '/we4062', # enumerator 'identifier' in switch of enum 'enumeration' is not handled
      '/we4098', # 'function' : void function returning a value
    ])
  endif
  add_project_arguments(msvc_args, language: 'c')
elif cc.has_link_argument('-Wl,-Bsymbolic-functions')
  # FIXME: Add an option for this if people ask for it
  add_project_link_arguments('-Wl,-Bsymbolic-functions', language : 'c')
endif

# glib doesn't support unloading, which means that unloading and reloading
# any library that registers static types will fail
if cc.has_link_argument('-Wl,-z,nodelete')
  add_project_link_arguments('-Wl,-z,nodelete', language: 'c')
endif

# Symbol visibility
have_visibility_hidden = false
if cc.has_argument('-fvisibility=hidden')
  add_project_arguments('-fvisibility=hidden', language: 'c')
  have_visibility_hidden = true
endif

# Disable strict aliasing
if cc.has_argument('-fno-strict-aliasing')
  add_project_arguments('-fno-strict-aliasing', language: 'c')
endif

# Define G_DISABLE_DEPRECATED for development versions
if gst_version_is_dev
  message('Disabling deprecated GLib API')
  add_project_arguments('-DG_DISABLE_DEPRECATED', language: 'c')
endif

# Same logic as in GLib.
glib_debug = get_option('glib_debug')
disable_cast_checks = glib_debug.disabled() or (
  glib_debug.auto() and (not get_option('debug') or get_option('optimization') not in [ '0', 'g' ]))
if disable_cast_checks
  message('Disabling GLib cast checks')
  add_project_arguments('-DG_DISABLE_CAST_CHECKS', language: 'c')
  disable_cast_checks = true
endif

disable_glib_asserts = not get_option('glib_assert')
if disable_glib_asserts
  message('Disabling GLib asserts')
  add_project_arguments('-DG_DISABLE_ASSERT', language: 'c')
endif

disable_glib_checks = not get_option('glib_checks')
if disable_glib_checks
  message('Disabling GLib checks')
  add_project_arguments('-DG_DISABLE_CHECKS', language: 'c')
endif

cdata.set_quoted('GST_API_VERSION', api_version)
cdata.set_quoted('GST_DATADIR', datadir)
cdata.set_quoted('LOCALEDIR', join_paths(prefix, get_option('localedir')))
cdata.set_quoted('LIBDIR', join_paths(prefix, get_option('libdir')))
cdata.set_quoted('GST_API_VERSION', '1.0')
cdata.set_quoted('GETTEXT_PACKAGE', 'gstreamer-1.0')
cdata.set_quoted('GST_LICENSE', 'LGPL')
cdata.set_quoted('PACKAGE', 'gstreamer')
cdata.set_quoted('PACKAGE_NAME', 'GStreamer')
cdata.set_quoted('PACKAGE_STRING', 'GStreamer @0@'.format(gst_version))
cdata.set_quoted('PACKAGE_TARNAME', 'gstreamer')
cdata.set_quoted('PACKAGE_BUGREPORT', 'https://gitlab.freedesktop.org/gstreamer/gstreamer/issues/new')
cdata.set_quoted('PACKAGE_URL', '')
cdata.set_quoted('PACKAGE_VERSION', gst_version)
cdata.set_quoted('PLUGINDIR', join_paths(get_option('prefix'), get_option('libdir'), 'gstreamer-1.0'))
cdata.set_quoted('VERSION', gst_version)
cdata.set_quoted('GST_PLUGIN_SCANNER_INSTALLED', join_paths(prefix, helpers_install_dir, 'gst-plugin-scanner'))
cdata.set_quoted('GST_PTP_HELPER_INSTALLED', join_paths(prefix, helpers_install_dir, 'gst-ptp-helper'))
cdata.set_quoted('GST_PLUGIN_SUBDIR', get_option('libdir'),
  description: 'plugin directory path component, used to find plugins on relocatable builds')
cdata.set_quoted('GST_PLUGIN_SCANNER_SUBDIR', libexecdir,
  description: 'libexecdir path component, used to find plugin-scanner on relocatable builds')
cdata.set_quoted('GST_PTP_HELPER_SUBDIR', libexecdir,
  description: 'libexecdir path component, used to find ptp-helper on relocatable builds')
cdata.set('GST_DISABLE_OPTION_PARSING', not get_option('option-parsing'))

mem_align_opt = get_option('memory-alignment')
if mem_align_opt == 'malloc'
  cdata.set('MEMORY_ALIGNMENT_MALLOC', 1)
elif mem_align_opt == 'pagesize'
  cdata.set('MEMORY_ALIGNMENT_PAGESIZE', 1)
else
  cdata.set('MEMORY_ALIGNMENT', mem_align_opt.to_int())
endif

if ['darwin', 'ios'].contains(host_system)
  cdata.set_quoted('GST_EXTRA_MODULE_SUFFIX', '.dylib')
endif

if gst_version_nano > 0
    # Have GST_ERROR message printed when running from git
    cdata.set('GST_LEVEL_DEFAULT', 'GST_LEVEL_ERROR')
else
    cdata.set('GST_LEVEL_DEFAULT', 'GST_LEVEL_NONE')
endif

# GStreamer package name and origin url
gst_package_name = get_option('package-name')
if gst_package_name == ''
  if gst_version_nano == 0
    gst_package_name = 'GStreamer source release'
  elif gst_version_nano == 1
    gst_package_name = 'GStreamer git'
  else
    gst_package_name = 'GStreamer prerelease'
  endif
endif
cdata.set_quoted('GST_PACKAGE_NAME', gst_package_name)
cdata.set_quoted('GST_PACKAGE_ORIGIN', get_option('package-origin'))

# These are only needed/used by the ABI tests
host_defines = [
  [ 'x86', 'HAVE_CPU_I386' ],
  [ 'x86_64', 'HAVE_CPU_X86_64' ],
  [ 'arm', 'HAVE_CPU_ARM' ],
  [ 'aarch64', 'HAVE_CPU_AARCH64' ],
  [ 'mips', 'HAVE_CPU_MIPS' ],
  [ 'powerpc', 'HAVE_CPU_PPC' ],
  [ 'powerpc64', 'HAVE_CPU_PPC64' ],
  [ 'alpha', 'HAVE_CPU_ALPHA' ],
  [ 'sparc', 'HAVE_CPU_SPARC' ],
  [ 'ia64', 'HAVE_CPU_IA64' ],
  [ 'hppa', 'HAVE_CPU_HPPA' ],
  [ 'm68k', 'HAVE_CPU_M68K' ],
  [ 's390', 'HAVE_CPU_S390' ],
]
foreach h : host_defines
  if h.get(0) == host_machine.cpu_family()
    cdata.set(h.get(1), 1)
  endif
endforeach
# FIXME: should really be called HOST_CPU or such
cdata.set_quoted('TARGET_CPU', host_machine.cpu())

check_headers = [
  'dlfcn.h',
  'inttypes.h',
  'memory.h',
  'poll.h',
  'stdint.h',
  'stdio_ext.h',
  'strings.h',
  'string.h',
  'sys/param.h',
  'sys/poll.h',
  'sys/prctl.h',
  'sys/socket.h',
  'sys/stat.h',
  'sys/times.h',
  'sys/time.h',
  'sys/types.h',
  'sys/utsname.h',
  'sys/wait.h',
  'ucontext.h',
  'unistd.h',
  'sys/resource.h',
  'sys/uio.h',
]

if host_system == 'windows'
  check_headers += ['winsock2.h']
endif

foreach h : check_headers
  if cc.has_header(h)
    define = 'HAVE_' + h.underscorify().to_upper()
    cdata.set(define, 1)
  endif
endforeach

if cc.has_member('struct tm', 'tm_gmtoff', prefix : '#include <time.h>')
  cdata.set('HAVE_TM_GMTOFF', 1)
endif

check_functions = [
  'gmtime_r',
  'sigaction',
  'getrusage',
  'fseeko',
  'ftello',
  'poll',
  'ppoll',
  'pselect',
  'getpagesize',
  'clock_gettime',
  'clock_nanosleep',
  'strnlen',
  # These are needed by libcheck
  'getline',
  'mkstemp',
  'alarm',
  'gettimeofday',
]

foreach f : check_functions
  if cc.has_function(f)
    define = 'HAVE_' + f.underscorify().to_upper()
    cdata.set(define, 1)
  endif
endforeach

if host_system == 'windows'
  if cc.has_function('_set_thread_local_invalid_parameter_handler', prefix : '#include <stdlib.h>')
    cdata.set('HAVE__SET_THREAD_LOCAL_INVALID_PARAMETER_HANDLER', 1)
  endif
endif

if cc.has_function('localtime_r', prefix : '#include<time.h>')
  cdata.set('HAVE_LOCALTIME_R', 1)
  # Needed by libcheck
  cdata.set('HAVE_DECL_LOCALTIME_R', 1)
endif

if cc.links('''#define _GNU_SOURCE
               #include <pthread.h>
               int main() {
                 pthread_setname_np("example"); return 0;
               }''', name : 'pthread_setname_np(const char*)')
  cdata.set('HAVE_PTHREAD_SETNAME_NP_WITHOUT_TID', 1)
endif
if cc.has_header_symbol('pthread.h', 'pthread_condattr_setclock')
  cdata.set('HAVE_PTHREAD_CONDATTR_SETCLOCK', 1)
endif
if cc.has_header_symbol('pthread.h', 'pthread_cond_timedwait_relative_np')
  cdata.set('HAVE_PTHREAD_COND_TIMEDWAIT_RELATIVE_NP', 1)
endif

# Check for futex(2)
if cc.compiles('''#include <linux/futex.h>
               #include <sys/syscall.h>
               #include <unistd.h>
               int main (int argc, char ** argv) {
                 syscall (__NR_futex, NULL, FUTEX_WAKE, FUTEX_WAIT);
                 return 0;
               }''', name : 'futex(2) system call')
  cdata.set('HAVE_FUTEX', 1)
endif
if cc.compiles('''#include <linux/futex.h>
               #include <sys/syscall.h>
               #include <unistd.h>
               int main (int argc, char ** argv) {
                 syscall (__NR_futex_time64, NULL, FUTEX_WAKE, FUTEX_WAIT);
                 return 0;
               }''', name : 'futex_time64(2) system call')
  cdata.set('HAVE_FUTEX_TIME64', 1)
endif

# Check for posix timers and monotonic clock
time_prefix = '#include <time.h>\n'
if cdata.has('HAVE_UNISTD_H')
  time_prefix += '#include <unistd.h>'
endif

posix_timers_src = time_prefix + '''
#if !defined(_POSIX_TIMERS) || _POSIX_TIMERS < 0 || !defined(CLOCK_REALTIME)
#error Either _POSIX_TIMERS or CLOCK_REALTIME not defined
#endif
'''
if cc.compiles(posix_timers_src, name : 'posix timers from time.h')
  cdata.set('HAVE_POSIX_TIMERS', 1)
endif

monotonic_clock_src = time_prefix + '''
#if !defined(_POSIX_MONOTONIC_CLOCK) || _POSIX_MONOTONIC_CLOCK < 0 || !defined(CLOCK_MONOTONIC)
#error Either _POSIX_MONOTONIC_CLOCK or CLOCK_MONOTONIC not defined
#endif
'''
if cc.compiles(monotonic_clock_src, name : 'monotonic clock from time.h')
  cdata.set('HAVE_MONOTONIC_CLOCK', 1)
endif

# Check for __uint128_t (gcc) by checking for 128-bit division
uint128_t_src = '''int main() {
static __uint128_t v1 = 100;
static __uint128_t v2 = 10;
static __uint128_t u;
u = v1 / v2;
}'''
if cc.compiles(uint128_t_src, name : '__uint128_t available')
  cdata.set('HAVE_UINT128_T', 1)
endif

# All supported platforms have long long now
cdata.set('HAVE_LONG_LONG', 1)

# -------------------------------------------------------------------------------------
# config.h things needed by libcheck
# -------------------------------------------------------------------------------------
if cc.has_function('getpid')
  cdata.set('HAVE_GETPID', 1)
elif host_system == 'windows' and cc.has_function('_getpid')
  cdata.set('HAVE_PROCESS_H', 1) # Used by gstreamer too
  cdata.set('HAVE__GETPID', 1)
endif
if cc.has_function('strdup')
  cdata.set('HAVE_DECL_STRDUP', 1)
elif host_system == 'windows' and cc.has_function('_strdup')
  cdata.set('HAVE__STRDUP', 1) # Windows (MSVC)
endif
if host_system != 'windows'
  cdata.set('HAVE_FORK', 1)
else
  # libcheck requires HAVE_FORK to be 0 when fork() is not available
  cdata.set('HAVE_FORK', 0)
endif
if cc.has_function('strsignal')
  cdata.set('HAVE_DECL_STRSIGNAL', 1)
endif
# Check for availability of types
if not cc.has_type('clockid_t', prefix : '#include <time.h>')
  cdata.set('clockid_t', 'int')
endif
if not cc.has_type('timer_t', prefix : '#include <time.h>')
  cdata.set('timer_t', 'int')
endif
if not cc.has_members('struct timespec', 'tv_sec', 'tv_nsec',
		      prefix : '#include <time.h>')
  cdata.set('STRUCT_TIMESPEC_DEFINITION_MISSING', 1)
endif
if not cc.has_members('struct itimerspec', 'it_interval', 'it_value',
		      prefix : '#include <time.h>')
  cdata.set('STRUCT_ITIMERSPEC_DEFINITION_MISSING', 1)
endif
if host_system != 'windows'
  cdata.set('HAVE_PIPE', 1)
elif cc.has_function('_pipe', prefix : '#include <io.h>')
  cdata.set('HAVE_PIPE', 1)
endif

# Platform deps; only ws2_32 and execinfo for now
platform_deps = []
if host_system == 'windows'
  platform_deps = [cc.find_library('ws2_32')]
endif

building_for_uwp = false
if host_system == 'windows'
  # Check whether we're building for UWP apps
  code = '''
  #include <windows.h>
  #if !(WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_APP) && !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP))
  #error "Not building for UWP"
  #endif'''
  if cc.compiles(code, name : 'building for UWP')
    building_for_uwp = true
  endif

  building_for_win7 = cc.compiles('''#include <windows.h>
      #ifndef WINVER
      #error "unknown minimum supported OS version"
      #endif
      #if (WINVER < _WIN32_WINNT_WIN7)
      #error "Windows 7 API is not guaranteed"
      #endif
      ''',
      name: 'building for Windows 7')

  if not building_for_win7
    add_project_arguments([
      '-U_WIN32_WINNT',
      '-UWINVER',
      '-D_WIN32_WINNT=_WIN32_WINNT_WIN7',
      '-DWINVER=_WIN32_WINNT_WIN7',
    ], language: ['c', 'cpp'])
  endif
endif

backtrace_deps = []
unwind_dep = dependency('libunwind', required : get_option('libunwind'))
dw_dep = dependency('libdw', required: get_option('libdw'))
dbghelp_option = get_option('dbghelp')
if dbghelp_option.enabled() and building_for_uwp
  error('DbgHelp is not supported for UWP')
endif
have_dbghelp = cc.has_header('dbghelp.h', required: dbghelp_option) and cc.has_header('tlhelp32.h', required: dbghelp_option)
backtrace_deps = [unwind_dep, dw_dep]
backtrace_source_info = false
backtrace_minimal = false
# MSVC debug stack trace support
if host_system == 'windows' and have_dbghelp and not building_for_uwp
  cdata.set('HAVE_DBGHELP', 1)
  backtrace_source_info = true
# DWARF stack trace support with libunwind and elf-utils
elif unwind_dep.found()
  cdata.set('HAVE_UNWIND', 1)
  if dw_dep.found()
    cdata.set('HAVE_DW', 1)
    backtrace_source_info = true
  endif
  backtrace_minimal = true
# Basic backtrace() stack trace support
elif cc.has_function('backtrace')
  cdata.set('HAVE_BACKTRACE', 1)
  backtrace_minimal = true
endif
# Print messages about what was enabled
if not backtrace_source_info
  if not backtrace_minimal
    message('NO support for stack traces.')
  else
    message('Minimal support for stack traces, no source info.')
  endif
endif

if cc.has_header('execinfo.h')
  if cc.has_function('backtrace', prefix : '#include <execinfo.h>')
    cdata.set('HAVE_BACKTRACE', 1)
  else
    execinfo_dep = cc.find_library('execinfo', required : false)
    if execinfo_dep.found() and cc.has_function('backtrace', prefix : '#include <execinfo.h>', dependencies : execinfo_dep)
      cdata.set('HAVE_BACKTRACE', 1)
      platform_deps += execinfo_dep
    endif
  endif
endif

gst_debug = get_option('gst_debug')
if not gst_debug
  if cc.get_argument_syntax() == 'msvc'
    msvc_args = cc.get_supported_arguments([
      '/wd4101', # 'identifier' : unreferenced local variable
      '/wd4189', # 'identifier' : local variable is initialized but not referenced
    ])
    add_project_arguments(msvc_args, language: 'c')
  elif cc.has_argument('-Wno-unused')
    add_project_arguments(['-Wno-unused'], language: 'c')
  endif
else
  if cc.get_argument_syntax() == 'msvc' and gst_version_is_dev
    msvc_args = cc.get_supported_arguments([
      '/we4101', # 'identifier' : unreferenced local variable
      '/we4189', # 'identifier' : local variable is initialized but not referenced
    ])
    add_project_arguments(msvc_args, language: 'c')
  endif
endif

# NOTE: Keep entries alphabetically sorted
warning_flags = [
  '-Waddress',
  '-Waggregate-return',
  '-Wformat',
  '-Wformat-nonliteral',
  '-Wformat-security',
  '-Wimplicit-fallthrough=3',
  '-Winit-self',
  '-Wmissing-declarations',
  '-Wmissing-include-dirs',
  '-Wmissing-parameter-type',
  '-Wmissing-prototypes',
  '-Wno-multichar',
  '-Wold-style-definition',
  '-Wpointer-arith',
  '-Wredundant-decls',
  '-Wshift-negative-value',
  '-Wtype-limits',
  '-Wundef',
  '-Wvla',
  '-Wwrite-strings',
]

foreach extra_arg : warning_flags
  if cc.has_argument (extra_arg)
    add_project_arguments([extra_arg], language: 'c')
  endif
endforeach

# Used by the gstutils test
gmp_dep = cc.find_library('gmp', required : false)
cdata.set('HAVE_GMP', gmp_dep.found())
gsl_dep = cc.find_library('gsl', required : false)
gslcblas_dep = cc.find_library('gslcblas', required : false)
cdata.set('HAVE_GSL', gsl_dep.found() and gslcblas_dep.found())
test_deps = [gmp_dep, gsl_dep, gslcblas_dep]

# Used by gstinfo.c
dl_dep = cc.find_library('dl', required : false)
cdata.set('HAVE_DLADDR', cc.has_function('dladdr', dependencies : dl_dep))
cdata.set('GST_ENABLE_EXTRA_CHECKS', not get_option('extra-checks').disabled())
cdata.set('USE_POISONING', get_option('poisoning'))

configinc = include_directories('.')
libsinc = include_directories('libs')
privinc = include_directories('gst')

glib_req = '>= 2.64.0'

# Find dependencies
glib_dep = dependency('glib-2.0', version: glib_req,
    default_options: {'sysprof': 'disabled'})
gobject_dep = dependency('gobject-2.0')
gmodule_dep = dependency('gmodule-no-export-2.0')
if host_system == 'windows'
    gio_dep = dependency('gio-2.0')
else
    gio_dep = [dependency('gio-2.0'),
               dependency('gio-unix-2.0')]
endif

mathlib = cc.find_library('m', required : false)
# Needed for timer_create/settime/delete
# Also provides clock_gettime in glibc < 2.17
rt_lib = cc.find_library('rt', required : false)

#
# Solaris and Illumos distros split a lot of networking-related code
# into '-lsocket -lnsl'.  Anything that calls socketpair(), getifaddr(),
# etc. probably needs to include network_deps
#
socket_lib = cc.find_library('socket', required: false)
nsl_lib    = cc.find_library('nsl',    required: false)
network_deps = []
if socket_lib.found()
  network_deps += socket_lib
endif
if nsl_lib.found()
  network_deps += nsl_lib
endif

# Check for libatomic for use of C11 atomics: some architectures need
# to link explicitly to this library.
# TODO: Once meson 1.7 is in use, this can be updated to
# dependency('atomic')
atomic_dep = cc.find_library('atomic', required: false)
platform_deps += atomic_dep

gir = find_program('g-ir-scanner', required : get_option('introspection'))
gnome = import('gnome')

build_gir = gir.found() and (not meson.is_cross_build() or get_option('introspection').enabled())

gir_init_section = [ '--add-init-section=extern void gst_init(gint*,gchar**);' + \
    'g_setenv("GST_REGISTRY_DISABLE", "yes", TRUE);' + \
    'g_setenv("GST_REGISTRY_1.0", "/no/way/this/exists.reg", TRUE);' + \
    'g_setenv("GST_PLUGIN_PATH_1_0", "", TRUE);' + \
    'g_setenv("GST_PLUGIN_SYSTEM_PATH_1_0", "", TRUE);' + \
    'g_setenv("GST_TRACERS", "", TRUE);' + \
    'gst_init(NULL,NULL);', '--quiet']

gst_c_args = ['-DHAVE_CONFIG_H']

# FIXME: This is only needed on windows and probably breaks when
# default_library = 'both'. We should add this flag to static_c_args instead
# when Meson supports it: https://github.com/mesonbuild/meson/issues/3304

if static_build
  gst_c_args += ['-DGST_STATIC_COMPILATION']
endif

# Used in gst/parse/meson.build and below
python3 = import('python').find_installation()

bashcomp_option = get_option('bash-completion')
bashcomp_dep = dependency('bash-completion', version : '>= 2.0', required : bashcomp_option)
bash_completions_dir = ''
bash_helpers_dir = ''

bashcomp_found = false
if bashcomp_dep.found()
  bashcomp_found = true
  bashcomp_dir_override = bashcomp_dep.version().version_compare('>= 2.10') ? ['datadir', datadir] : ['prefix', prefix]
  bash_completions_dir = bashcomp_dep.get_variable('completionsdir', pkgconfig_define:  bashcomp_dir_override)
  if bash_completions_dir == ''
    msg = 'Found bash-completion but the .pc file did not set \'completionsdir\'.'
    if bashcomp_option.enabled()
      error(msg)
    else
      message(msg)
    endif
    bashcomp_found = false
  endif

  bash_helpers_dir = bashcomp_dep.get_variable('helpersdir', pkgconfig_define:  bashcomp_dir_override)
  if bash_helpers_dir == ''
    msg = 'Found bash-completion, but the .pc file did not set \'helpersdir\'.'
    if bashcomp_option.enabled()
      error(msg)
    else
      message(msg)
    endif
    bashcomp_found = false
  endif
endif

plugins_install_dir = join_paths(get_option('libdir'), 'gstreamer-1.0')

pkgconfig = import('pkgconfig')
plugins_pkgconfig_install_dir = join_paths(plugins_install_dir, 'pkgconfig')
if get_option('default_library') == 'shared'
  # If we don't build static plugins there is no need to generate pc files
  plugins_pkgconfig_install_dir = disabler()
endif
pkgconfig_variables = [
  'exec_prefix=${prefix}',
  'toolsdir=${exec_prefix}/bin',
  'pluginsdir=${libdir}/gstreamer-1.0',
  'girdir=${datadir}/gir-1.0',
  'typelibdir=${libdir}/girepository-1.0',
  'pluginscannerdir=${libexecdir}/gstreamer-1.0'
]
pkgconfig_uninstalled_variables = ['exec_prefix=${prefix}',
	'gstreamerdir=${prefix}/subprojects/gstreamer',
    'bashhelpersdir=${gstreamerdir}/data/bash-completion/helpers',
    'helpersdir=${gstreamerdir}/libs/gst/helpers']
pkgconfig_subdirs = ['gstreamer-1.0']
pkgconfig_libs = []
if host_system == 'darwin'
  pkgconfig_libs = ['-Wl,-rpath,${libdir}']
endif

if host_machine.system() == 'windows'
  pathsep = ';'
else
  pathsep = ':'
endif

gst_libraries = []
subdir('cmake')
subdir('gst')
subdir('libs')
subdir('plugins')
subdir('tools')
subdir('tests')
subdir('data')
subdir('docs')

# xgettext is optional (on Windows for instance)
if find_program('xgettext', required : get_option('nls')).found()
  cdata.set('ENABLE_NLS', 1)
  subdir('po')
endif

subdir('scripts')

# Set release date
if gst_version_nano == 0
  extract_release_date = find_program('scripts/extract-release-date-from-doap-file.py')
  run_result = run_command(extract_release_date, gst_version, files('gstreamer.doap'), check: true)
  release_date = run_result.stdout().strip()
  cdata.set_quoted('GST_PACKAGE_RELEASE_DATETIME', release_date)
  message('Package release date: ' + release_date)
endif

configure_file(output : 'config.h', configuration : cdata)
install_data('gst-element-check-1.0.m4', install_dir : join_paths(get_option('datadir'), 'aclocal'))

meson.add_dist_script('scripts/gen-changelog.py', meson.project_name(), '1.24.0', meson.project_version())

plugin_names = []
gst_plugins = []
foreach plugin: plugins
  pkgconfig.generate(plugin, install_dir: plugins_pkgconfig_install_dir)
  dep = declare_dependency(link_with: plugin, variables: {'full_path': plugin.full_path()})
  meson.override_dependency(plugin.name(), dep)
  gst_plugins += [dep]
  if plugin.name().startswith('gst')
    plugin_names += [plugin.name().substring(3)]
  else
    plugin_names += [plugin.name()]
  endif
endforeach

summary({
    'Plugins': plugin_names,
}, list_sep: ', ')
