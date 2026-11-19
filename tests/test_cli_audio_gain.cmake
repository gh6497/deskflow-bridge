# Exercise validation without trying to open an ADB or HID device.
function(expect_error expected)
  execute_process(COMMAND "${CLI}" ${ARGN} RESULT_VARIABLE status ERROR_VARIABLE error)
  if(NOT "${status}" STREQUAL "2" OR NOT error MATCHES "${expected}")
    message(FATAL_ERROR "${ARGN}: expected status 2 and '${expected}', got ${status}: ${error}")
  endif()
endfunction()

foreach(invalid IN ITEMS -1 13 1.5 6dB nope 999999999999999999999)
  expect_error("requires an integer from 0 to 12" --audio --audio-gain-db "${invalid}")
endforeach()
expect_error("requires an integer from 0 to 12" --audio --audio-gain-db)
expect_error("--audio-gain-db requires --audio" --audio-gain-db 0)
if(AUDIO)
  foreach(valid IN ITEMS 0 6 12)
    expect_error("require --scrcpy-server and --scrcpy-version" --audio-gain-db "${valid}"
      --audio --adb-serial FAKE_PHONE)
  endforeach()
else()
  expect_error("audio support was not built" --audio --audio-gain-db 6 --adb-serial FAKE_PHONE
    --scrcpy-server fake-server --scrcpy-version 5.0.1)
endif()
