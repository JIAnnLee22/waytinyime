subdir('code_table')
subdir('wayland')
subdir('ui')
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
    input_method_dependency,
    xim_frontend_dependency,
  ],
  install: true,
)

install_data(
  flypy_table,
  install_dir: join_paths(get_option('datadir'), meson.project_name()),
)
