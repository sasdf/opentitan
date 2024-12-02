def _rust_stdlib_repo_impl(repository_ctx):
    repository_ctx.download_and_extract(
        url = "https://static.rust-lang.org/dist/{date}/rustc-{channel}-src.tar.gz".format(
            date = repository_ctx.attr.date,
            channel = repository_ctx.attr.channel,
        ),
        type = "tar.gz",
        stripPrefix = "rustc-{channel}-src".format(channel = repository_ctx.attr.channel),
    )

    repository_ctx.file(
        "BUILD",
        content = repository_ctx.read(repository_ctx.attr.build_template),
    )

rust_stdlib_repo = repository_rule(
    implementation = _rust_stdlib_repo_impl,
    attrs = {
        "date": attr.string(mandatory = True),
        "channel": attr.string(mandatory = True),
        "build_template": attr.label(mandatory = True),
    },
)
