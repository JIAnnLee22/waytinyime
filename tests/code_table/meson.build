code_table_test = executable(
  'code-table-test',
  'code_table_test.c',
  dependencies: code_table_dependency,
)

test(
  'code-table',
  code_table_test,
  args: [
    flypy_table,
    files('invalid-code.txt'),
  ],
)
