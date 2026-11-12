#pragma once

// Copy this pair of files into your own integration directory and replace the
// TODO bodies with calls into the camera vendor's C++ SDK. Enable the build with
//   -DSPIKE_USE_VENDOR_SHIM=ON -DSPIKE_VENDOR_SHIM_DIR=<your dir>
// The rest of the application never includes a vendor header.
namespace spike {
void registerVendorSdkShim();
}
