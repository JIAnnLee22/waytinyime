table_config_library = static_library(
  'table-config',
  'table_config.c',
  dependencies: [xkbcommon],
)

table_config_dependency = declare_dependency(
  include_directories: include_directories('.'),
  link_with: table_config_library,
  dependencies: [xkbcommon],
)
