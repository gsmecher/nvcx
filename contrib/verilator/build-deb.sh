#!/bin/sh
# Build nvcx's Verilator package: the upstream source upstream.ref names (a
# release tag or a commit), with the packaging and patches in debian/
# (Debian's own, carried here until the distributions nvcx supports ship a
# recent enough Verilator).
#
#   build-deb.sh OUTDIR [SUFFIX]
#
# Run as root in a container of the target distribution (it installs the
# build dependencies). The .deb lands in OUTDIR, renamed
# verilator_<version>_<SUFFIX>.deb if SUFFIX is given (as the release
# workflow names packages). DEB_BUILD_OPTIONS adds to the defaults: nodoc
# always (the manual needs TeX), and e.g. nocheck to skip upstream's smoke
# tests and examples. By hand, for example:
#
#   docker run --rm -v $PWD/contrib/verilator:/src:ro -v $PWD/out:/out \
#       debian:13 /src/build-deb.sh /out
#
# To move to another upstream version: put its tag (v5.056) or commit in
# upstream.ref, add a debian/changelog entry whose upstream version names it
# (5.056, or 5.055+git202611201234.1a2b3c4 for a commit after 5.055, with
# the commit's UTC date and time), and replace upstream.sha256 with the new
# tarball's checksum (the script prints it on a mismatch). Patches go in debian/patches (quilt, DEP-3 headers), listed in
# debian/patches/series.

set -eu

here=$(cd "$(dirname "$0")" && pwd)
out=$1
suffix=${2:-}
mkdir -p "$out"
out=$(cd "$out" && pwd)

export DEBIAN_FRONTEND=noninteractive
apt-get -y update
apt-get -y install --no-install-recommends ca-certificates dpkg-dev wget

version=$(dpkg-parsechangelog -l "$here/debian/changelog" -S Version)
upstream=${version%-*}
upstream=${upstream#*:}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
tarball=verilator_$upstream.orig.tar.gz
wget -q -O "$tarball" \
    "https://github.com/verilator/verilator/archive/$(cat "$here/upstream.ref").tar.gz"
want=$(cat "$here/upstream.sha256")
got=$(sha256sum "$tarball" | cut -d' ' -f1)
if [ "$got" != "$want" ]; then
    echo "$tarball: sha256 $got, expected $want (upstream.sha256)" >&2
    exit 1
fi
mkdir "verilator-$upstream"
tar xzf "$tarball" -C "verilator-$upstream" --strip-components=1
cp -r "$here/debian" "verilator-$upstream/debian"

cd "verilator-$upstream"
export DEB_BUILD_PROFILES=nodoc
export DEB_BUILD_OPTIONS="nodoc parallel=$(nproc) ${DEB_BUILD_OPTIONS:-}"
apt-get -y build-dep ./
# (applies debian/patches first: dpkg-source --before-build)
dpkg-buildpackage -uc -us -b

deb=verilator_${version#*:}_$(dpkg --print-architecture).deb
if [ -n "$suffix" ]; then
    cp "../$deb" "$out/verilator_${version#*:}_$suffix.deb"
else
    cp "../$deb" "$out/"
fi
