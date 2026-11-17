# ==============================================================================
# Cloud-Native Multi-Stage Containerfile for ps-printer-app
# Optimized for: Universal Blue (Bluefin/Bazzite), Fedora Silverblue, and Podman
# ==============================================================================

# ------------------------------------------------------------------------------
# Stage 1: Build Environment
# ------------------------------------------------------------------------------
FROM fedora:41 AS builder

RUN dnf install -y \
    gcc \
    gcc-c++ \
    make \
    autoconf \
    automake \
    libtool \
    pkgconf-pkg-config \
    git \
    cups-devel \
    libjpeg-turbo-devel \
    libpng-devel \
    libtiff-devel \
    libusb1-devel \
    avahi-devel \
    gnutls-devel \
    pam-devel \
    qpdf-devel \
    poppler-devel \
    zlib-devel \
    foomatic-db \
    foomatic-db-ppds \
    && dnf clean all

WORKDIR /build

# Build PAPPL 2.x (master)
RUN git clone --depth 1 https://github.com/michaelrsweet/pappl.git \
    && cd pappl \
    && ./configure --prefix=/usr --enable-libusb --with-dnssd=avahi \
    && make -j$(nproc) \
    && make install

# Build libcupsfilters
RUN git clone --depth 1 https://github.com/OpenPrinting/libcupsfilters.git \
    && cd libcupsfilters \
    && ./autogen.sh --prefix=/usr \
    && make -j$(nproc) \
    && make install

# Build libppd
RUN git clone --depth 1 https://github.com/OpenPrinting/libppd.git \
    && cd libppd \
    && ./autogen.sh --prefix=/usr \
    && make -j$(nproc) \
    && make install

# Build pappl-retrofit
RUN git clone --depth 1 https://github.com/OpenPrinting/pappl-retrofit.git \
    && cd pappl-retrofit \
    && ./autogen.sh --prefix=/usr \
    && make -j$(nproc) \
    && make install

# Compile ps-printer-app
COPY . /build/ps-printer-app
WORKDIR /build/ps-printer-app
RUN make -j$(nproc) && make install DESTDIR=/build/rootfs

# ------------------------------------------------------------------------------
# Stage 2: Minimal Cloud-Native Runtime Image
# ------------------------------------------------------------------------------
FROM fedora-minimal:41

# Runtime dependencies only
RUN microdnf install -y \
    cups-libs \
    libusb1 \
    libpng \
    libjpeg-turbo \
    libtiff \
    avahi-libs \
    gnutls \
    qpdf-libs \
    poppler \
    foomatic-db \
    foomatic-db-ppds \
    shadow-utils \
    && microdnf clean all

# Copy compiled OpenPrinting stack and application
COPY --from=builder /usr/lib64/libpappl* /usr/lib64/
COPY --from=builder /usr/lib64/libcupsfilters* /usr/lib64/
COPY --from=builder /usr/lib64/libppd* /usr/lib64/
COPY --from=builder /usr/lib64/libpappl-retrofit* /usr/lib64/
COPY --from=builder /build/rootfs/ /

# Dedicated unprivileged service account
RUN useradd -r -u 1001 -g lp -d /var/run/ps-printer-app -s /sbin/nologin printer-app \
    && mkdir -p /var/run/ps-printer-app /var/spool/ps-printer-app /var/lib/ps-printer-app \
    && chown -R 1001:lp /var/run/ps-printer-app /var/spool/ps-printer-app /var/lib/ps-printer-app

USER 1001:lp
WORKDIR /var/run/ps-printer-app

# Expose default IPP / PAPPL web service port
EXPOSE 8000

# Direct PID 1 execution (no shell wrapper, no background zombies)
ENTRYPOINT ["/usr/bin/ps-printer-app"]
CMD ["-o", "server-port=8000", "server"]
