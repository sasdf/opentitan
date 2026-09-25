import os

files = [
    "sw/device/tests/usbdev_iso_test.c",
    "sw/device/tests/usbdev_mixed_test.c",
    "sw/device/tests/usbdev_stream_test.c"
]

for f in files:
    with open(f, "r") as file:
        content = file.read()
    
    # Revert my bad sed patch
    bad_code = "    case kDeviceSimQemu:\n      verbose = true;\n      break;\n"
    if bad_code in content:
        content = content.replace(bad_code, "")
    
    # Find `case kDeviceSimVerilator:` and add `case kDeviceSimQemu:` right above it
    if "case kDeviceSimQemu" not in content:
        content = content.replace("case kDeviceSimVerilator:", "case kDeviceSimQemu:\n    case kDeviceSimVerilator:")
    
    with open(f, "w") as file:
        file.write(content)

