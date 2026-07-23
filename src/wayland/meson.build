keyboard_grab_library = static_library(
  'keyboard-grab',
  'keyboard_grab.c',
  include_directories: include_directories('../..'),
  dependencies: [wayland_client, xkbcommon],
)

keyboard_grab_dependency = declare_dependency(
  include_directories: include_directories('.'),
  link_with: keyboard_grab_library,
  dependencies: [wayland_client, xkbcommon],
)
