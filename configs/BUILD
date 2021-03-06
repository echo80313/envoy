licenses(["notice"])  # Apache 2

load(
    "//bazel:envoy_build_system.bzl",
    "envoy_package",
)

envoy_package()

load("//bazel:envoy_build_system.bzl", "envoy_py_test_binary")

envoy_py_test_binary(
    name = "configgen",
    srcs = ["configgen.py"],
    data = glob([
        "*.yaml",
        "*.json",
    ]),
    external_deps = ["jinja2"],
)

filegroup(
    name = "configs",
    srcs = [
        "original-dst-cluster/proxy_config.yaml",
    ] + select({
        "//bazel:apple": [],
        "//conditions:default": ["freebind/freebind.yaml"],
    }),
)

genrule(
    name = "v1_upgraded_configs",
    srcs = ["google_com_proxy.yaml"],
    outs = ["google_com_proxy.v2.upgraded.json"],
    cmd = "$(location //tools:v1_to_bootstrap) $(location google_com_proxy.yaml) > $@",
    tools = ["//tools:v1_to_bootstrap"],
)

genrule(
    name = "example_configs",
    srcs = [
        ":configs",
        ":v1_upgraded_configs",
        "//examples:configs",
        "//test/config/integration/certs",
    ],
    outs = ["example_configs.tar"],
    cmd = "$(location configgen.sh) $(location configgen) $(@D) $(locations :configs) " +
          "$(locations //examples:configs) $(locations //test/config/integration/certs)",
    tools = [
        "configgen.sh",
        ":configgen",
    ],
)
