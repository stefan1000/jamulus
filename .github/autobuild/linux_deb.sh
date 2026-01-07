#!/bin/bash
##############################################################################
# Copyright (c) 2022-2025
#
# Author(s):
#  Christian Hoffmann
#  The Jamulus Development Team
#
##############################################################################
#
# This program is free software; you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free Software
# Foundation; either version 2 of the License, or (at your option) any later
# version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
# details.
#
# You should have received a copy of the GNU General Public License along with
# this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
#
##############################################################################

set -eu

if [[ ! ${JAMULUS_BUILD_VERSION:-} =~ [0-9]+\.[0-9]+\.[0-9]+ ]]; then
    echo "Environment variable JAMULUS_BUILD_VERSION has to be set to a valid version string"
    exit 1
fi

TARGET_ARCH="${TARGET_ARCH:-amd64}"
case "${TARGET_ARCH}" in
    amd64)
        ABI_NAME=""
        ;;
    armhf)
        ABI_NAME="arm-linux-gnueabihf"
        ;;
    arm64)
        ABI_NAME="aarch64-linux-gnu"
        ;;
    *)
        echo "Unsupported TARGET_ARCH ${TARGET_ARCH}"
        exit 1
        ;;
esac

setup() {
    export DEBIAN_FRONTEND="noninteractive"

    APT_ARCH="$(dpkg --print-architecture)"
    export APT_ARCH

    echo "Installing dependencies..."
    apt-get -qq update
    apt-get -qq --no-install-recommends -y install devscripts build-essential debhelper fakeroot libjack-jackd2-dev qt6-base-dev qt6-tools-dev-tools qt6-tools-dev qt6-tools-dev-tools qt6-multimedia-dev
}


build_app_as_deb() {
    # Add Qt6 qmake to PATH
    export PATH="/usr/lib/qt6/bin:$PATH"

    TARGET_ARCH="${TARGET_ARCH}" ./linux/deploy_deb.sh
}

pass_artifacts_to_job() {
    mkdir deploy

    # rename headless first, so wildcard pattern matches only one file each
    local artifact_1="jamulus-headless_${JAMULUS_BUILD_VERSION}_ubuntu_${TARGET_ARCH}.deb"
    echo "Moving headless build artifact to deploy/${artifact_1}"
    mv ../jamulus-headless*"_${TARGET_ARCH}.deb" "./deploy/${artifact_1}"
    echo "artifact_1=${artifact_1}" >> "$GITHUB_OUTPUT"

    local artifact_2="jamulus_${JAMULUS_BUILD_VERSION}_ubuntu_${TARGET_ARCH}.deb"
    echo "Moving regular build artifact to deploy/${artifact_2}"
    mv ../jamulus*_"${TARGET_ARCH}.deb" "./deploy/${artifact_2}"
    echo "artifact_2=${artifact_2}" >> "$GITHUB_OUTPUT"
}

case "${1:-}" in
    setup)
        setup
        ;;
    build)
        build_app_as_deb
        ;;
    get-artifacts)
        pass_artifacts_to_job
        ;;
    *)
        echo "Unknown stage '${1:-}'"
        exit 1
        ;;
esac
