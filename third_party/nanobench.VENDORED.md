# nanobench vendored snapshot

Upstream: https://github.com/martinus/nanobench
Pinned commit: e4327893194f06928012eb81cabc606c4e4791ac
License: MIT (license header at the top of nanobench.h)

Single-header microbenchmark library. Vendored as a single header (the
upstream `src/include/nanobench.h`) to match the existing convention for
`stb_image.h` / `vk_mem_alloc.h` (see sibling files under `third_party/`).

Update procedure:

    git clone --depth 1 https://github.com/martinus/nanobench.git /tmp/nb
    cp /tmp/nb/src/include/nanobench.h \
        src/backend_scene/third_party/nanobench.h
    # Re-record the pinned commit above; re-run the bench leg
    # (`scripts/preflight.sh --bench`) and expect a small but non-zero
    # CSV drift (nanobench refines warmup + rng between versions).
