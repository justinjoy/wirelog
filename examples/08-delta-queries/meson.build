# examples/08-delta-queries/meson.build
# Delta Queries demo using the wl_easy facade

delta_demo = executable(
  'delta_demo',
  files('delta_demo.c'),
  link_with: wirelog_static_lib,
  include_directories: [wirelog_inc, wirelog_src_inc],
  dependencies: [nanoarrow_dep, threads_dep, xxhash_dep, mbedtls_dep, math_dep],
  install: false,
)
