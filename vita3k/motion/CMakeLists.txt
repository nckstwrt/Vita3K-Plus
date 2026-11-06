add_library(
	motion
	STATIC
	src/motion.cpp
	src/motion_input.cpp
)

target_include_directories(motion PUBLIC include)
target_link_libraries(motion PUBLIC emuenv SDL3::SDL3 util)
target_link_libraries(motion PRIVATE config ctrl)
if(ANDROID)
    target_link_libraries(motion PRIVATE android)
endif()

if(NOT ANDROID)
    add_executable(trigger_tilt_test EXCLUDE_FROM_ALL tests/trigger_tilt_test.cpp)
    target_link_libraries(trigger_tilt_test PRIVATE motion config ctrl)
    add_test(NAME trigger_tilt COMMAND trigger_tilt_test)
endif()
