FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates build-essential cmake ninja-build pkg-config libsdl2-dev python3 \
    && rm -rf /var/lib/apt/lists/*
COPY esp32/simulator/CMakeLists.txt esp32/simulator/lv_conf.h /src/esp32/simulator/
COPY esp32/simulator/compat /src/esp32/simulator/compat
COPY esp32/simulator/include /src/esp32/simulator/include
COPY esp32/simulator/src /src/esp32/simulator/src
COPY esp32/components/muse /src/esp32/components/muse
COPY esp32/avatar /src/esp32/avatar
COPY linux/lvgl/*.c linux/lvgl/*.h /src/linux/lvgl/
COPY esp32/simulator/build-spin/_deps/lvgl-src /opt/lvgl
RUN cmake -S /src/esp32/simulator -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DFETCHCONTENT_SOURCE_DIR_LVGL=/opt/lvgl \
    && cmake --build /build --target muse_linux_app --parallel 8 \
    && ctest --test-dir /build -R muse_linux_app_smoke --output-on-failure
FROM muse-mascot-x11:local
USER root
COPY --from=build /build/muse_linux_app /usr/local/bin/muse_linux_app
ENV SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy SDL_FRAMEBUFFER_ACCELERATION=0 SDL_RENDER_DRIVER=software
WORKDIR /tmp
ENTRYPOINT ["muse_linux_app"]
CMD []
