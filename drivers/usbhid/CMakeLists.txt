# USB HID Driver for Apple IIe Emulator on RP2350
# Provides keyboard and gamepad input via USB Host
#
# SPDX-License-Identifier: MIT

add_library(usbhid INTERFACE)

# Check if USB HID is enabled
if(USB_HID_ENABLED)
    message(STATUS "USB HID Host enabled")
    message(STATUS "  NOTE: USB CDC stdio will be DISABLED when USB HID is enabled!")
    
    # Add the actual source files when enabled
    target_sources(usbhid INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/hid_app.c
        ${CMAKE_CURRENT_LIST_DIR}/usbhid_wrapper.c
    )
    
    # Include this directory for tusb_config.h
    target_include_directories(usbhid INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}
    )
    
    target_compile_definitions(usbhid INTERFACE
        USB_HID_ENABLED=1
    )
    
    # Link TinyUSB Host
    target_link_libraries(usbhid INTERFACE
        tinyusb_host
        tinyusb_board
    )
else()
    message(STATUS "USB HID Host disabled (use -DUSB_HID_ENABLED=1 to enable)")
    message(STATUS "  USB CDC stdio remains available for debug output")
    # Stub functions are provided inline in usbhid_wrapper.h
    # Do NOT include the usbhid directory to avoid tusb_config conflicts
endif()
