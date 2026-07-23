candidate_popup_library = static_library(
  'candidate-popup',
  'candidate_popup.c',
  include_directories: include_directories('../..'),
  dependencies: [wayland_client, fontconfig, freetype, code_table_dependency],
)

candidate_popup_dependency = declare_dependency(
  include_directories: include_directories('.'),
  link_with: candidate_popup_library,
  dependencies: [wayland_client, fontconfig, freetype, code_table_dependency],
)
