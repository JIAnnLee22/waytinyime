input_method_library = static_library(
  'input-method',
  [
    'input_engine.c',
    'input_processor.c',
    'input_session.c',
  ],
  include_directories: include_directories('../..'),
  dependencies: [
    code_table_dependency,
    candidate_popup_dependency,
    keyboard_grab_dependency,
  ],
)

input_method_dependency = declare_dependency(
  include_directories: include_directories('.'),
  link_with: input_method_library,
  dependencies: [
    code_table_dependency,
    candidate_popup_dependency,
    keyboard_grab_dependency,
  ],
)
