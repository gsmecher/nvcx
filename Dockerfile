# Ubuntu 24.04 (an LTS release Vivado supports)
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update -qq && apt-get install -y --no-install-recommends \
		autoconf \
		automake \
		build-essential \
		ca-certificates \
		cmake \
		flex \
		libboost-context-dev \
		libdw-dev \
		libffi-dev \
		libpixman-1-0 \
		libpng16-16 \
		libreadline-dev \
		libudev1 \
		libunwind8 \
		libx11-6 \
		llvm-18-dev \
		locales \
		make \
		patch \
		pkg-config \
		python3 \
		python3-hypothesis \
		python3-pytest \
		python3-pytest-xdist \
		tcl8.6-dev \
		tcllib \
		wget \
		zlib1g-dev && \
	rm -rf /var/lib/apt/lists/*

# Vivado asks for libtinfo.so.5 and libncurses.so.5, which 24.04 no longer ships
RUN ln -s libtinfo.so.6 /usr/lib/x86_64-linux-gnu/libtinfo.so.5
RUN ln -s libncurses.so.6 /usr/lib/x86_64-linux-gnu/libncurses.so.5

# Build NVC with tcl enabled and our patches applied (some change the build
# system, hence autoreconf)
ARG NVC_VERSION=1.23.0
RUN wget -q https://github.com/nickg/nvc/releases/download/r${NVC_VERSION}/nvc-${NVC_VERSION}.tar.gz -O /tmp/nvc.tar.gz && \
	mkdir -p /tmp/nvc && \
	tar xzf /tmp/nvc.tar.gz -C /tmp/nvc --strip-components=1
COPY contrib/*.patch /tmp/patches/
RUN cd /tmp/nvc && \
	for p in /tmp/patches/*.patch; do \
		patch -p1 < $p || exit 1; \
	done && \
	autoreconf -fi && \
	mkdir build && \
	cd build && \
	../configure --prefix=/usr/local \
		--with-llvm=/usr/bin/llvm-config-18 \
		--enable-tcl && \
	make -j"$(nproc)" && \
	make install && \
	rm -rf /tmp/nvc /tmp/nvc.tar.gz /tmp/patches

RUN sed -i '/en_US.UTF-8/s/^# //g' /etc/locale.gen && locale-gen
ENV LANG=en_US.UTF-8
