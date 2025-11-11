#!/usr/bin/env python3

import argparse
import base64
import gzip
import json
from pathlib import Path

def main():
    parser = argparse.ArgumentParser(description='Bundles coverage JSON files into the viewer HTML.')
    parser.add_argument('--viewer_html', type=Path, default='util/coverage/viewer.html',
                        help='Path to the viewer HTML file.')
    parser.add_argument('--coverage_json', type=Path, default='bazel-out/_coverage/coverage.json.gz',
                        help='Path to the gzipped JSON coverage data file.')
    parser.add_argument('--view_json', type=Path, default='bazel-out/_coverage/views.json.gz',
                        help='Path to the gzipped JSON view data file.')
    parser.add_argument('--output_html', type=Path, default='bazel-out/_coverage/viewer/index.html',
                        help='Path to the output HTML file with bundled data.')
    args = parser.parse_args()

    # Read the viewer HTML template
    viewer_html_content = args.viewer_html.read_text()

    # Read and base64 encode the gzipped coverage JSON
    coverage_data_gz = args.coverage_json.read_bytes()
    coverage_data_b64 = base64.b64encode(coverage_data_gz).decode('utf-8')

    # Read and base64 encode the gzipped view JSON
    view_data_gz = args.view_json.read_bytes()
    view_data_b64 = base64.b64encode(view_data_gz).decode('utf-8')

    # Prepare the bundled data JavaScript
    bundled_data_js = f"""
    // -- Bundled data --
    bundledData.set('coverage.json.gz', `
      {coverage_data_b64}
    `);
    bundledData.set('view.json.gz', `
      {view_data_b64}
    `);
    // -- End bundled data --
    """

    # Insert bundled data into the HTML
    output_html_content = viewer_html_content.replace('// -- Bundled data --', bundled_data_js)

    # Write the output HTML file
    args.output_html.parent.mkdir(parents=True, exist_ok=True)
    args.output_html.write_text(output_html_content)

    print(f"Bundled coverage data into {args.output_html}")

if __name__ == '__main__':
    main()
