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

    subparsers = parser.add_subparsers(dest='command', required=True)

    # Subcommand: bundle
    bundle_parser = subparsers.add_parser('bundle', help='Bundle individual files.')
    bundle_parser.add_argument('--coverage_json', type=Path, default='bazel-out/_coverage/coverage.json.gz',
                        help='Path to the gzipped JSON coverage data file.')
    bundle_parser.add_argument('--view_json', type=Path, default='bazel-out/_coverage/views.json.gz',
                        help='Path to the gzipped JSON view data file.')
    bundle_parser.add_argument('--output_html', type=Path, default='bazel-out/_coverage/viewer/index.html',
                        help='Path to the output HTML file with bundled data.')

    # Subcommand: update
    update_parser = subparsers.add_parser('update', help='Update and re-bundle the viewer using data from a directory.')
    update_parser.add_argument('update_dir', type=Path,
                        help='Directory containing coverage.json.gz and view.json.gz.')

    args = parser.parse_args()

    # Read the viewer HTML template
    viewer_html_content = args.viewer_html.read_text()

    if args.command == 'update':
        coverage_json_path = args.update_dir / 'coverage.json.gz'
        view_json_path = args.update_dir / 'view.json.gz'
        output_html_path = args.update_dir / 'index.html'
    else:
        coverage_json_path = args.coverage_json
        view_json_path = args.view_json
        output_html_path = args.output_html

    # Read and base64 encode the gzipped coverage JSON
    coverage_data_gz = coverage_json_path.read_bytes()
    coverage_data_b64 = base64.b64encode(coverage_data_gz).decode('utf-8')

    # Read and base64 encode the gzipped view JSON
    view_data_gz = view_json_path.read_bytes()
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
    output_html_path.parent.mkdir(parents=True, exist_ok=True)
    output_html_path.write_text(output_html_content)

    print(f"Bundled coverage data into {output_html_path}")

if __name__ == '__main__':
    main()
