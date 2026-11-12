cmake_minimum_required(VERSION 3.21)

project(SpikeCalibrationQt VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

if(COMMAND qt_standard_project_setup)
  qt_standard_project_setup()
endif()

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Widgets)
find_package(OpenCV REQUIRED COMPONENTS core imgproc calib3d imgcodecs)

# Optional: link the camera vendor's C++ SDK. The runtime adapter itself is ABI-neutral
# (a C function-pointer table), so the core build never needs the vendor headers.
set(SPIKE_VENDOR_SDK_INCLUDE_DIR "" CACHE PATH "Vendor C++ SDK include directory (optional)")
set(SPIKE_VENDOR_SDK_LIBRARY "" CACHE FILEPATH "Vendor C++ SDK import library (optional)")
option(SPIKE_BUILD_SELFTEST "Build the console geometry self-test" ON)
option(SPIKE_USE_VENDOR_SHIM "Compile the integrator-provided VendorSdkShim.h" OFF)
set(SPIKE_VENDOR_SHIM_DIR "" CACHE PATH "Directory containing VendorSdkShim.h (optional)")

set(SPIKE_CORE_HEADERS
  src/core/Common.h
  src/core/Model.h
  src/core/Targets.h
  src/core/Importers.h
  src/core/Calibration.h
  src/core/Fixtures.h
  src/core/Workbench.h
  src/core/CalibrationSDK.h
  src/device/ICameraDevice.h
  src/device/FileCameraDevice.h
  src/device/VendorSdkCameraDevice.h
  src/device/CameraDeviceFactory.h
  src/device/DeviceController.h
)
set(SPIKE_CORE_SOURCES
  src/core/Common.cpp
  src/core/Model.cpp
  src/core/Targets.cpp
  src/core/Importers.cpp
  src/core/Calibration.cpp
  src/core/Fixtures.cpp
  src/core/Workbench.cpp
  src/core/CalibrationSDK.cpp
  src/device/FileCameraDevice.cpp
  src/device/VendorSdkCameraDevice.cpp
  src/device/CameraDeviceFactory.cpp
  src/device/DeviceController.cpp
)

# Algorithm/persistence/device layer: Qt Core+Gui only, no Widgets dependency.
add_library(spike_core STATIC ${SPIKE_CORE_SOURCES} ${SPIKE_CORE_HEADERS})
target_include_directories(spike_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(spike_core PUBLIC Qt6::Core Qt6::Gui ${OpenCV_LIBS})
target_compile_definitions(spike_core PUBLIC SPIKE_VERSION=\"${PROJECT_VERSION}\")
if(SPIKE_VENDOR_SDK_INCLUDE_DIR)
  target_include_directories(spike_core PUBLIC ${SPIKE_VENDOR_SDK_INCLUDE_DIR})
endif()
if(SPIKE_VENDOR_SDK_LIBRARY)
  target_link_libraries(spike_core PUBLIC ${SPIKE_VENDOR_SDK_LIBRARY})
endif()

set(SPIKE_UI_HEADERS
  src/ui/ImageViewer.h
  src/ui/UiHelpers.h
  src/ui/CapturePanel.h
  src/ui/IntrinsicsPanel.h
  src/ui/TurntablePanel.h
  src/ui/ConfigPanel.h
  src/ui/DevicePanel.h
  src/ui/MainWindow.h
)
set(SPIKE_UI_SOURCES
  src/ui/ImageViewer.cpp
  src/ui/UiHelpers.cpp
  src/ui/CapturePanel.cpp
  src/ui/IntrinsicsPanel.cpp
  src/ui/TurntablePanel.cpp
  src/ui/ConfigPanel.cpp
  src/ui/DevicePanel.cpp
  src/ui/MainWindow.cpp
  src/app/main.cpp
)

add_executable(SpikeCalibrationQt WIN32 ${SPIKE_UI_SOURCES} ${SPIKE_UI_HEADERS})
target_link_libraries(SpikeCalibrationQt PRIVATE spike_core Qt6::Widgets)
set_target_properties(SpikeCalibrationQt PROPERTIES OUTPUT_NAME "SpikeCalibrationTool")
if(SPIKE_USE_VENDOR_SHIM)
  target_compile_definitions(SpikeCalibrationQt PRIVATE SPIKE_USE_VENDOR_SHIM=1)
  if(NOT SPIKE_VENDOR_SHIM_DIR)
    set(SPIKE_VENDOR_SHIM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/examples")
  endif()
  target_include_directories(SpikeCalibrationQt PRIVATE ${SPIKE_VENDOR_SHIM_DIR})
  target_sources(SpikeCalibrationQt PRIVATE "${SPIKE_VENDOR_SHIM_DIR}/VendorSdkShim.cpp")
endif()

if(SPIKE_BUILD_SELFTEST)
  add_executable(spike_selftest tests/selftest.cpp)
  target_link_libraries(spike_selftest PRIVATE spike_core)
  enable_testing()
  add_test(NAME geometry_self_test COMMAND spike_selftest
    --data "${CMAKE_BINARY_DIR}/selftest_workspace"
    --output "${CMAKE_BINARY_DIR}/selftest_output")
endif()

if(WIN32)
  # windeployqt is invoked manually after build; see README.md.
  target_compile_definitions(spike_core PUBLIC NOMINMAX)
endif()
