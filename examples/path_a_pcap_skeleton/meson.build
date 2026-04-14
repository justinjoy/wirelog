# examples/path_a_pcap_skeleton/meson.build
#
# Standalone meson project that builds against *installed* libwirelog.
# This is NOT a subdirectory of the main wirelog build — it is compiled
# as a separate project in CI to catch header-install regressions (#449).
#
# CI usage (see ci-pr.yml):
#   meson install -C builddir --destdir=$PWD/staging
#   PKG_CONFIG_PATH=staging/.../pkgconfig meson setup ... path_a_pcap_skeleton
#   meson compile -C builddir-path-a

project(
  'path_a_pcap_skeleton',
  'c',
  version: '0.1.0',
  meson_version: '>=0.62.0',
  default_options: ['warning_level=3', 'c_std=c11'],
)

wirelog_dep = dependency('wirelog', required: true)

executable(
  'pcap_skeleton',
  files('main.c'),
  dependencies: [wirelog_dep],
  install: false,
)
