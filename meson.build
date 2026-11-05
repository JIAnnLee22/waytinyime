project(
  'waytinyime',
  ['c', 'cpp'],
  version: '0.1.0',
  default_options: [
    'c_std=c11',
    'cpp_std=c++20',
    'warning_level=3',
  ],
)

wayland_client = dependency('wayland-client')
xkbcommon = dependency('xkbcommon')
fontconfig = dependency('fontconfig')
freetype = dependency('freetype2')
xcb_imdkit = dependency('xcb-imdkit')
xcb_keysyms = dependency('xcb-keysyms')
libime_table = dependency(
  'LibIMETable',
  method: 'cmake',
  modules: ['LibIME::Table'],
)
wayland_scanner = find_program('wayland-scanner', native: true)

protocols = [
  ['input-method-v2', 'protocols/input-method-unstable-v2.xml'],
  ['virtual-keyboard-v1', 'protocols/virtual-keyboard-unstable-v1.xml'],
]

protocol_sources = []
foreach protocol : protocols
  name = protocol[0]
  xml = protocol[1]

  protocol_sources += custom_target(
    name + '-header',
    input: xml,
    output: name + '.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
  protocol_sources += custom_target(
    name + '-code',
    input: xml,
    output: name + '.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  )
endforeach

subdir('src')
subdir('tests')
