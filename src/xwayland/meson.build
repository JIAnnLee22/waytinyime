xim_frontend_library = static_library(
  'xim-frontend',
  ['xim_frontend.c', 'xim_window.c'],
  include_directories: include_directories('../..'),
  dependencies: [
    xcb_imdkit,
    xcb_keysyms,
    fontconfig,
    freetype,
    emoji_font_dependency,
    input_method_dependency,
    code_table_dependency,
  ],
)

xim_frontend_dependency = declare_dependency(
  include_directories: include_directories('.'),
  link_with: xim_frontend_library,
  dependencies: [
    xcb_imdkit,
    xcb_keysyms,
    fontconfig,
    freetype,
    emoji_font_dependency,
    input_method_dependency,
    code_table_dependency,
  ],
)
