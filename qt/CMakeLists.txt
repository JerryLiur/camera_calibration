cmake_minimum_required(VERSION 3.21)

project(SpikeCalibrationQt VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(COMMAND qt_standard_project_setup)
  qt_standard_project_setup()
endif()

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Widgets)
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)
# OpenCV 5 renamed opencv_calib3d to opencv_calib and moved chessboard detection
# into opencv_objdetect; calib3d.hpp remains available as a compatibility header.
if(OpenCV_VERSION VERSION_GREATER_EQUAL 5)
  list(APPEND OpenCV_LIBS opencv_calib opencv_objdetect)
else()
  list(APPEND OpenCV_LIBS opencv_calib3d)
endif()

# Optional: link the camera vendor's C++ SDK. The runtime adapter itself is ABI-neutral
# (a C function-pointer table), so the core build never needs the vendor headers.
set(SPIKE_VENDOR_SDK_INCLUDE_DIR "" CACHE PATH "Vendor C++ SDK include directory (optional)")
set(SPIKE_VENDOR_SDK_LIBRARY "" CACHE FILEPATH "Vendor C++ SDK import library (optional)")
option(SPIKE_BUILD_SELFTEST "Build the console geometry self-test" ON)
option(SPIKE_BUILD_TESTS "Build the mock-data integration and Widgets smoke tests" ON)
option(SPIKE_REQUIRE_MOCK_DATA "Fail configuration when release validation data is missing" OFF)
option(SPIKE_USE_VENDOR_SHIM "Compile the integrator-provided VendorSdkShim.h" OFF)
set(SPIKE_VENDOR_SHIM_DIR "" CACHE PATH "Directory containing VendorSdkShim.h (optional)")
set(SPIKE_MOCK_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../output/mock_calibration_data"
    CACHE PATH "Mock dataset directory (scripts/generate_mock_calibration_data.py)")
if(SPIKE_REQUIRE_MOCK_DATA AND NOT EXISTS "${SPIKE_MOCK_DATA_DIR}/manifest.json")
  message(FATAL_ERROR "Generate mock data before release validation: scripts/generate_mock_calibration_data.py")
endif()

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
)

# Widgets layer as a library so the smoke test can build the real MainWindow.
add_library(spike_ui STATIC ${SPIKE_UI_SOURCES} ${SPIKE_UI_HEADERS})
target_link_libraries(spike_ui PUBLIC spike_core Qt6::Widgets)

add_executable(SpikeCalibrationQt WIN32 src/app/main.cpp ${SPIKE_UI_HEADERS} resources/app.qrc)
if(WIN32)
  configure_file(resources/app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)
  target_sources(SpikeCalibrationQt PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/app.rc")
endif()
target_link_libraries(SpikeCalibrationQt PRIVATE spike_ui)
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
  if(EXISTS "${SPIKE_MOCK_DATA_DIR}/manifest.json")
    add_test(NAME geometry_self_test COMMAND spike_selftest
      --data "${CMAKE_BINARY_DIR}/selftest_workspace"
      --output "${CMAKE_BINARY_DIR}/selftest_output"
      --observations "${SPIKE_MOCK_DATA_DIR}/scenes/clean"
      --ground-truth "${SPIKE_MOCK_DATA_DIR}/ground_truth.json")
  else()
    add_test(NAME geometry_self_test COMMAND spike_selftest
      --data "${CMAKE_BINARY_DIR}/selftest_workspace"
      --output "${CMAKE_BINARY_DIR}/selftest_output")
  endif()
endif()

if(SPIKE_BUILD_TESTS)
  enable_testing()

  add_executable(spike_integration tests/integration.cpp)
  target_link_libraries(spike_integration PRIVATE spike_core)
  add_test(NAME mock_data_integration COMMAND spike_integration
    --mock-data "${SPIKE_MOCK_DATA_DIR}"
    --data "${CMAKE_BINARY_DIR}/integration_workspace"
    --output "${CMAKE_BINARY_DIR}/integration_output")
  set_tests_properties(mock_data_integration PROPERTIES SKIP_RETURN_CODE 77)

  # Builds the real MainWindow offscreen: panel wiring and stale-project recovery.
  add_executable(spike_uitest tests/uitest.cpp resources/app.qrc)
  target_link_libraries(spike_uitest PRIVATE spike_ui)
  add_test(NAME widgets_smoke COMMAND spike_uitest)
  set_tests_properties(widgets_smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

  if(WIN32)
    # CTest starts the executables directly, so Qt/OpenCV DLLs and the platform
    # plugins must be reachable without a system-wide PATH entry.
    get_target_property(_spike_qt_core_location Qt6::Core IMPORTED_LOCATION_RELEASE)
    if(NOT _spike_qt_core_location)
      get_target_property(_spike_qt_core_location Qt6::Core IMPORTED_LOCATION)
    endif()
    if(_spike_qt_core_location)
      get_filename_component(_spike_qt_bin "${_spike_qt_core_location}" DIRECTORY)
      get_filename_component(_spike_qt_prefix "${_spike_qt_bin}" DIRECTORY)
      # ENVIRONMENT is a CMake list: PATH separators must remain inside one item.
      string(REPLACE ";" "\\;" _spike_test_path "${_spike_qt_bin};$ENV{PATH}")
      set_tests_properties(mock_data_integration PROPERTIES
        ENVIRONMENT "PATH=${_spike_test_path}")
      if(SPIKE_BUILD_SELFTEST)
        set_tests_properties(geometry_self_test PROPERTIES ENVIRONMENT "PATH=${_spike_test_path}")
      endif()
      set_tests_properties(widgets_smoke PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;PATH=${_spike_test_path};QT_PLUGIN_PATH=${_spike_qt_prefix}/plugins")
    endif()
  endif()
endif()

if(WIN32)
  # windeployqt is invoked manually after build; see README.md.
  target_compile_definitions(spike_core PUBLIC NOMINMAX)
endif()
