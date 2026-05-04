# examples/13-daemon-style/meson.build
# Daemon-style session rotation demo (#629)
#
# Include all wirelog sources directly (same pattern as example 08 and
# wirelog_cli) to ensure proper linking under LTO.

daemon_style_demo = executable(
  'daemon_style_demo',
  files('daemon.c'),
  wirelog_sources,
  config_h_file,
  include_directories: [wirelog_inc, wirelog_src_inc],
  dependencies: [threads_dep, zlib_dep, nanoarrow_dep, xxhash_dep, mbedtls_dep, math_dep],
  install: false,
)

test(
  'example_13_daemon_style_smoke',
  daemon_style_demo,
  args: ['--events', '96', '--log-every', '32'],
  suite: 'examples',
  env: {'WIRELOG_COMPOUND_MAX_EPOCHS': '32'},
  is_parallel: false,
)

test(
  'example_13_daemon_style_paced',
  daemon_style_demo,
  args: [
    '--events', '1200',
    '--seconds', '10',
    '--log-every', '256',
    '--sleep-ms', '10',
  ],
  suite: 'examples',
  env: {'WIRELOG_COMPOUND_MAX_EPOCHS': '64'},
  is_parallel: false,
  timeout: 30,
)
