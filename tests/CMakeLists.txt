function(sse_add_test name)
  add_executable(test_${name} test_${name}.c)
  target_link_libraries(test_${name} PRIVATE eventsource)
  target_compile_options(test_${name} PRIVATE -Wall -Wextra -Werror)
  add_test(NAME ${name} COMMAND test_${name})
endfunction()

sse_add_test(parser_init)
sse_add_test(parser_basic)
