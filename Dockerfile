# syntax=docker/dockerfile:1
#
#   docker build -t order-matching-engine .                 # runtime image
#   docker build --target test -t order-matching-engine:test .   # build + run the test suite
#
# SRS 2.3 targets Linux; this builds with the distro's GCC.

# ---- toolchain + sources ----------------------------------------------------
FROM ubuntu:24.04 AS base
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential cmake ninja-build git ca-certificates \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY include include
COPY src src
COPY tests tests

# ---- release build (no tests, portable codegen) -----------------------------
# -march=native is disabled: the image may run on a different CPU than the one
# that built it.
FROM base AS build
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DOMS_NATIVE_ARCH=OFF -DOMS_BUILD_TESTS=OFF \
 && cmake --build build

# ---- test stage: `docker build --target test .` fails if any test fails -----
FROM base AS test
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DOMS_NATIVE_ARCH=OFF \
 && cmake --build build \
 && ctest --test-dir build --output-on-failure

# ---- runtime image ----------------------------------------------------------
FROM ubuntu:24.04 AS runtime
COPY --from=build /src/build/order_matching_engine /usr/local/bin/order_matching_engine
USER ubuntu
EXPOSE 8080
ENTRYPOINT ["order_matching_engine"]
CMD ["--host", "0.0.0.0", "--port", "8080"]
