xim_frontend_library = static_library(
  'xim-frontend',
  'xim_frontend.c',
  dependencies: [xcb_imdkit, xcb_keysyms, input_method_dependency],
)

xim_frontend_dependency = declare_dependency(
  include_directories: include_directories('.'),
  link_with: xim_frontend_library,
  dependencies: [xcb_imdkit, xcb_keysyms, input_method_dependency],
)
