# Shader library tools: g++ (C++23) and spirv-val. Debian trixie because the
# official Linux DXC release needs glibc 2.38+. DXC itself is mounted from
# .tools/dxc (see build_library.sh).
FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends g++ spirv-tools \
    && rm -rf /var/lib/apt/lists/*
