table_source_library = static_library(
  'table-source',
  [
    'table_source.c',
    '../code_table/libime_bridge.cc',
  ],
  include_directories: include_directories('..'),
  dependencies: [
    code_table_dependency,
    table_config_dependency,
    libime_table,
  ],
  link_language: 'cpp',
)

table_source_dependency = declare_dependency(
  include_directories: include_directories('.'),
  link_with: table_source_library,
  dependencies: [
    code_table_dependency,
    table_config_dependency,
    libime_table,
  ],
)
