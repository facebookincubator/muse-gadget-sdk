# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (see LICENSE).
FROM debian:bookworm-slim AS runtime
ENV DEBIAN_FRONTEND=noninteractive PYTHONUNBUFFERED=1 PATH=/opt/muse/venv/bin:$PATH
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates python3 python3-venv python3-cryptography python3-dbus python3-gi \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --create-home --uid 1000 --shell /bin/bash muse \
    && python3 -m venv --system-site-packages /opt/muse/venv
COPY linux/ /opt/muse/source/
RUN pip install --require-hashes -r /opt/muse/source/src/musegadget/data/requirements.lock \
    && pip install --no-deps /opt/muse/source
COPY docker-mac/entrypoint.sh /usr/local/bin/muse-entrypoint
RUN chmod 755 /usr/local/bin/muse-entrypoint
WORKDIR /home/muse
ENTRYPOINT ["muse-entrypoint"]
CMD ["run", "--run-as", "muse"]

FROM runtime AS test
RUN pip install pytest
WORKDIR /opt/muse/source
ENTRYPOINT ["python", "-m", "pytest"]
CMD ["-q"]
