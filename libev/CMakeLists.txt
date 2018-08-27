
add_definitions(-DEV_STANDALONE=1)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

set(EV_SRC 
  ev.c
  event.c
  )

add_library(ev-cetus SHARED ${EV_SRC})

CHASSIS_INSTALL_TARGET(ev-cetus)
