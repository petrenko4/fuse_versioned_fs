FROM debian:stable-slim

ENV DEBIAN_FRONTEND=noninteractive

# Install dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    gcc \
    g++ \
    gdb \
    nano \
    vim \
    make \
    pkg-config \
    fuse3 \
    libfuse3-dev \
    sudo \
    git \
    curl \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Allow FUSE inside container
RUN echo "user_allow_other" >> /etc/fuse.conf

# Create vscode user
RUN useradd -ms /bin/bash vscode \
    && echo "vscode ALL=(ALL) NOPASSWD:ALL" >> /etc/sudoers

# Create workspace structure
WORKDIR /workspaces/fuse_versioned_fs

# Copy project source files into src/
COPY . .

# Set ownership
RUN chown -R vscode:vscode /workspaces

USER vscode

WORKDIR /workspaces/fuse_versioned_fs/

CMD ["/bin/bash"]