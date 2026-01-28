#!/usr/bin/env python3
"""
Convert config_page.html to a gzipped C header file for embedding.
Similar to how static_html_hex.h is generated for the terminal UI.
"""

import gzip
import sys
import os

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    input_file = os.path.join(script_dir, 'config_page.html')
    output_file = os.path.join(script_dir, 'config_page_hex.h')
    
    # Read the HTML file
    with open(input_file, 'r') as f:
        html_content = f.read()
    
    # Compress with gzip
    compressed = gzip.compress(html_content.encode('utf-8'), compresslevel=9)
    
    # Generate C header
    with open(output_file, 'w') as f:
        f.write('/* Auto-generated from captive_portal/config_page.html */\n')
        f.write('#ifndef CONFIG_PAGE_HEX_H\n')
        f.write('#define CONFIG_PAGE_HEX_H\n\n')
        f.write('#include <stddef.h>\n\n')
        f.write('static const unsigned char config_page_gz[] __attribute__((aligned(4))) = {\n')
        
        # Write bytes in hex format, 12 per line
        for i, byte in enumerate(compressed):
            if i % 12 == 0:
                f.write('  ')
            f.write(f'0x{byte:02x}')
            if i < len(compressed) - 1:
                f.write(', ')
            if i % 12 == 11:
                f.write('\n')
        
        if len(compressed) % 12 != 0:
            f.write('\n')
        
        f.write('};\n\n')
        f.write(f'static const size_t config_page_gz_len = {len(compressed)};\n\n')
        f.write('#endif /* CONFIG_PAGE_HEX_H */\n')
    
    print(f'Generated {output_file}')
    print(f'Original size: {len(html_content)} bytes')
    print(f'Compressed size: {len(compressed)} bytes')
    print(f'Compression ratio: {len(compressed)/len(html_content)*100:.1f}%')

if __name__ == '__main__':
    main()
