project('audacious-plugin-ytdlp', 'cpp',
  version: '0.1.0',
  default_options: ['cpp_std=c++17']
)

# Audacious development headers / pkg-config
audacious_dep = dependency('audacious')

cxx = meson.get_compiler('cpp')

# Default hidden visibility — only aud_plugin_instance is exported.
# Matches the convention used by official Audacious transport plugins.
add_project_arguments(cxx.get_supported_arguments(['-fvisibility=hidden']), language: 'cpp')

# PACKAGE is used by PluginInfo.domain for gettext (unused here but required).
add_project_arguments('-DPACKAGE="audacious-plugin-ytdlp"', language: 'cpp')

# Resolve the Audacious plugin directory from pkg-config.
transport_plugin_dir = audacious_dep.get_variable(pkgconfig: 'plugin_dir') / 'Transport'

# Build as a shared module (not a library) so Audacious can dlopen it.
# name_prefix: '' ensures the output is ytdlp.so, not libytdlp.so.
shared_module('ytdlp',
  'src/common.cc',
  'src/stream.cc',
  'src/plugin.cc',
  dependencies: [audacious_dep],
  name_prefix: '',
  install: true,
  install_dir: transport_plugin_dir
)
