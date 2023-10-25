def kj_test(
        src,
        data = [],
        deps = [],
        tags = []):
    test_name = src.removesuffix(".c++")
    native.cc_test(
        name = test_name,
        srcs = [src],
        linkstatic = select({
          "@platforms//os:linux": 0,
          "@platforms//os:windows": 0,
          "//conditions:default": 1,
        }),
        deps = [
            "@capnp-cpp//src/kj:kj-test",
        ] + select({
            "@platforms//os:windows": [],
            "//conditions:default": ["@workerd//src/workerd/util:symbolizer"],
        }) + deps,
        linkopts = select({
            "@//:use_dead_strip": ["-Wl,-dead_strip", "-Wl,-no_exported_symbols"],
            "//conditions:default": [""],
        }),
    )
