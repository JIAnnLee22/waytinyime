subdir('code_table')
subdir('config')
subdir('wayland')
subdir('ui')
subdir('table_source')
subdir('input_method')
subdir('xwayland')

flypy_table = files('assets/flypy.txt')

waytinyime_configuration = configuration_data()
waytinyime_configuration.set_quoted(
  'WAYTINYIME_TABLE_PATH',
  join_paths(
    get_option('prefix'),
    get_option('datadir'),
    meson.project_name(),
    'flypy.txt',
  ),
)
waytinyime_configuration.set_quoted(
  'WAYTINYIME_BUILD_TABLE_PATH',
  join_paths(meson.current_source_dir(), 'assets', 'flypy.txt'),
)
waytinyime_configuration.set_quoted(
  'WAYTINYIME_EMOJI_PATH',
  join_paths(get_option('prefix'), get_option('datadir'), meson.project_name(), 'emoji.txt'),
)
waytinyime_configuration.set_quoted(
  'WAYTINYIME_BUILD_EMOJI_PATH',
  join_paths(meson.current_source_dir(), 'assets', 'emoji.txt'),
)
configure_file(
  output: 'waytinyime-config.h',
  configuration: waytinyime_configuration,
)

executable(
  'waytinyime',
  ['main.c', protocol_sources],
  dependencies: [
    wayland_client,
    code_table_dependency,
    table_source_dependency,
    input_method_dependency,
    xim_frontend_dependency,
  ],
  install: true,
  link_language: 'cpp',
)

install_data(
  flypy_table,
  files('assets/emoji.txt', 'assets/UNICODE-LICENSE.txt'),
  install_dir: join_paths(get_option('datadir'), meson.project_name()),
)
