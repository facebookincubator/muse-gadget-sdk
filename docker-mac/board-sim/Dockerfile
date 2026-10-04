# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (see LICENSE).
FROM debian:bookworm-slim
ENV DEBIAN_FRONTEND=noninteractive PYTHONUNBUFFERED=1 PATH=/opt/musegadget/venv/bin:$PATH
RUN apt-get update && apt-get install -y --no-install-recommends \
    bash ca-certificates openssh-server python3 python3-venv python3-cryptography \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --create-home --shell /bin/bash muse \
    && python3 -m venv --system-site-packages /opt/musegadget/venv
COPY linux/ /opt/musegadget/source/
RUN pip install --require-hashes -r /opt/musegadget/source/src/musegadget/data/requirements.lock \
    && pip install --no-deps /opt/musegadget/source
COPY docker-mac/board-sim/sshd_config /etc/ssh/sshd_config
COPY docker-mac/board-sim/entrypoint.sh /usr/local/bin/board-sim-entrypoint
RUN chmod 755 /usr/local/bin/board-sim-entrypoint
# Simulator-only dependency for the published synthetic phone/board test peers.
RUN pip install 'pytest>=7,<9'
WORKDIR /home/muse
ENTRYPOINT ["board-sim-entrypoint"]
