# Terminal Web Interface

This directory contains the web-based terminal interface for the Altair 8800 emulator.

## Files

- `index.html` - Main HTML file containing the terminal interface
- `update_static_html.sh` - Script to convert HTML to C header file

## Updating the HTML Interface

When you make changes to `index.html`, you **must** regenerate the static HTML hex file that gets compiled into the firmware:

### Update Process

1. Make your changes to `index.html` or other web assets
2. Run the update script from this directory:
   ```bash
   ./update_static_html.sh
   ```
3. This will generate a new `../static_html_hex.h` header file containing the HTML as a C byte array
4. Rebuild the firmware to include the updated web interface

### What the Script Does

The `update_static_html.sh` script:
- Converts `index.html` into a hexadecimal byte array
- Generates the `static_html_hex.h` header file in the parent directory
- This header is included during compilation so the HTML is embedded in the firmware

### Important Notes

- Always run `update_static_html.sh` after modifying web interface files
- The generated header file must be committed to version control
- Without running this script, your HTML changes won't be included in the firmware
