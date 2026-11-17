FROM fedora:41 AS builder

RUN dnf install -y \
    gcc \
    gcc-c++ \
    make \
    which \
    autoconf \
    automake \
    libtool \
    gettext-devel \
    pkgconf-pkg-config \
    git \
    cups-devel \
    libjpeg-turbo-devel \
    libpng-devel \
    libtiff-devel \
    libjxl-devel \
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

RUN git clone --depth 1 --recurse-submodules https://github.com/OpenPrinting/libcups.git \
    && cd libcups \
    && ./configure --prefix=/usr \
    && make -j$(nproc) \
    && make install

RUN git clone --depth 1 https://github.com/michaelrsweet/pappl.git \
    && cd pappl \
    && ./configure --prefix=/usr --enable-libusb --with-dnssd=avahi \
    && make -j$(nproc) \
    && make install

RUN git clone --depth 1 https://github.com/OpenPrinting/libcupsfilters.git \
    && cd libcupsfilters \
    && ./autogen.sh \
    && ./configure --prefix=/usr \
    && make -j$(nproc) \
    && make install

RUN git clone --depth 1 https://github.com/OpenPrinting/libppd.git \
    && cd libppd \
    && ./autogen.sh \
    && ./configure --prefix=/usr \
    && make -j$(nproc) \
    && make install

RUN git clone --depth 1 https://github.com/OpenPrinting/pappl-retrofit.git \
    && cd pappl-retrofit \
    && ./autogen.sh \
    && ./configure --prefix=/usr \
    && make -j$(nproc) \
    && make install

COPY . /build/ps-printer-app
WORKDIR /build/ps-printer-app
RUN make -j$(nproc) && make install DESTDIR=/build/rootfs

FROM quay.io/fedora/fedora-minimal:41

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

COPY --from=builder /usr/lib64/libpappl* /usr/lib64/
COPY --from=builder /usr/lib64/libcupsfilters* /usr/lib64/
COPY --from=builder /usr/lib64/libppd* /usr/lib64/
COPY --from=builder /usr/lib64/libpappl-retrofit* /usr/lib64/
COPY --from=builder /build/rootfs/ /

RUN useradd -r -u 1001 -g lp -d /var/run/ps-printer-app -s /sbin/nologin printer-app \
    && mkdir -p /var/run/ps-printer-app /var/spool/ps-printer-app /var/lib/ps-printer-app \
    && chown -R 1001:lp /var/run/ps-printer-app /var/spool/ps-printer-app /var/lib/ps-printer-app

USER 1001:lp
WORKDIR /var/run/ps-printer-app

EXPOSE 8000

ENTRYPOINT ["/usr/bin/ps-printer-app"]
CMD ["-o", "server-port=8000", "server"]
