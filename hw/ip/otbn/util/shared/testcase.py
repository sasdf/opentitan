# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

import hjson
import re


def _is_numeric(value):
  return bool(re.match(r'(0x)?[0-9a-fA-F\s]+$', value))

def parse_testcase(data: str, symbols={}) -> dict:
  data = hjson.loads(data)

  entrypoint = data.get('entrypoint', '0')
  if _is_numeric(entrypoint):
    data['entrypoint'] = int(entrypoint, 0)
  else:
    data['entrypoint'] = symbols[entrypoint]

  for key in ['input', 'output']:
    data.setdefault(key, {})
    data[key].setdefault('dmem', {})
    data[key].setdefault('regs', {})

    for label, value in data[key]['dmem'].items():
      value = value.strip()
      if not _is_numeric(value):
        value = symbols[value]
        data[key]['dmem'][label] = value.to_bytes(4, 'little')
      elif value.startswith('0x'):
        # big-endian int -> little-endian byte array
        data[key]['dmem'][label] = bytes.fromhex(value[2:])[::-1]
      else:
        # byte array
        data[key]['dmem'][label] = bytes.fromhex(value)

    for label, value in data[key]['regs'].items():
      value = value.strip()
      if not _is_numeric(value):
        data[key]['regs'][label] = symbols[value]
      else:
        data[key]['regs'][label] = int(value, 0)

  return data
