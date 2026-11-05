emoji_font_library = static_library(
  'emoji-font',
  'emoji_font.c',
  dependencies: [fontconfig, freetype],
)
emoji_font_dependency = declare_dependency(
  include_directories: include_directories('.'),
  link_with: emoji_font_library,
  dependencies: [fontconfig, freetype],
)

candidate_popup_library = static_library(
  'candidate-popup',
  'candidate_popup.c',
  include_directories: include_directories('../..'),
  dependencies: [wayland_client, fontconfig, freetype, code_table_dependency, emoji_font_dependency],
)

candidate_popup_dependency = declare_dependency(
  include_directories: include_directories('.'),
  link_with: candidate_popup_library,
  dependencies: [wayland_client, fontconfig, freetype, code_table_dependency, emoji_font_dependency],
)
