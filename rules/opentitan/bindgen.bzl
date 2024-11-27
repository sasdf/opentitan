# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

"""Helper for creating bindings for on-device rust lib."""

load("@rules_rust//rust:defs.bzl", "rust_library")
load("@rules_rust//bindgen:defs.bzl", "rust_bindgen_library")
load(
    "//rules:cross_platform.bzl",
    "dual_cc_device_library_of",
    "dual_cc_library",
    "dual_inputs",
)


def ot_rust_library(name, crate_name=None, hdrs=[], rustc_flags=[], **kwargs):
  if crate_name == None:
      crate_name = '//' + native.package_name() + '_' + name
      crate_name = crate_name.replace('/', '_')

  rust_library(
      name=name,
      crate_name=crate_name,
      rustc_flags = rustc_flags + [
          "-C", "opt-level=s",
      ],
      **kwargs)

  if len(hdrs):
      native.cc_library(
          name = name+"_cc",
          hdrs = hdrs,
          deps = [":" + name],
      )


def ot_bindgen(name, crate_name=None, bindgen_flags=[], rustc_flags=[], **kwargs):
  if crate_name == None:
      crate_name = '//' + native.package_name() + '_' + name
      crate_name = crate_name.replace('/', '_')
  rust_bindgen_library(
      name = name,
      crate_name = crate_name,
      bindgen_flags = bindgen_flags + [
          "--raw-line=#![no_std]",
          "--use-core",
          "--no-prepend-enum-name",
      ],
      rustc_flags = rustc_flags + [
          "-C", "opt-level=s",
          "--allow=non_snake_case",
          "--allow=non_camel_case_types",
          "--allow=nonstandard_style",
          "--allow=non_upper_case_globals",
      ],
      **kwargs,
  )

def cc_library_with_bindgen(name, hdrs=[], bindgen={}, **kwargs):
  if len(hdrs) == 0:
    fail("Multiple headers is not supported.")

  native.cc_library(name=name, hdrs=hdrs, **kwargs)

  cc_lib = ":"+name
  ot_bindgen(
      name = name + "_bindgen",
      cc_lib = cc_lib,
      header = ":"+hdrs[0],
      **bindgen,
  )

def dual_cc_library_with_bindgen(name, bindgen = {}, hdrs = [], **kwargs):

    hdrs = hdrs if type(hdrs) != "list" else dual_inputs(shared = hdrs)
    hdrs_d = hdrs.shared + hdrs.device

    dual_cc_library(name=name, hdrs=hdrs, **kwargs)

    cc_lib = dual_cc_device_library_of(name)
    ot_bindgen(
        name = name + "_bindgen",
        cc_lib = cc_lib,
        header = ":"+hdrs_d[0],
        **bindgen,
    )
