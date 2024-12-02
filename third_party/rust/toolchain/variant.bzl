def _rust_toolchain_transition_impl(settings, attr):
    return {
        "//third_party/rust/toolchain:variant": attr.variant,
        "@rules_rust//rust/toolchain/channel:channel": attr.channel,
    }

_rust_toolchain_transition = transition(
    inputs = [],
    outputs = [
        "//third_party/rust/toolchain:variant",
        "@rules_rust//rust/toolchain/channel:channel",
    ],
    implementation = _rust_toolchain_transition_impl,
)

def _with_rust_toolchain_transition_impl(ctx):
    return [DefaultInfo(files = depset(ctx.files.srcs))]

with_rust_toolchain = rule(
    implementation = _with_rust_toolchain_transition_impl,
    attrs = {
        "srcs": attr.label_list(
            allow_files = True,
        ),
        "variant": attr.string(),
        "channel": attr.string(),
        "_allowlist_function_transition": attr.label(
            default = Label("@bazel_tools//tools/allowlists/function_transition_allowlist"),
        ),
    },
    cfg = _rust_toolchain_transition,
)
