# syntax=docker/dockerfile:1

# The source of this image is available at https://github.com/breezewish/tiflash-devcontainer
FROM breezewish/tiflash-dev:0.0.3

# Create directories to mount as volumes. These directories will be persisted
# after the container is recreated.
RUN mkdir -p /root/.vscode-server/extensions \
    && mkdir -p /root/.cargo/registry \
    && mkdir -p /root/.cargo/git \
    && mkdir -p /root/.rustup/toolchains \
    && mkdir -p /root/.cache/ccache \
    && mkdir -p /root/.config/gh
