#!/usr/bin/env bash

# Script run inside docker by test-repo.sh
set -eu
ABSOLUTE_PATH=$(cd `dirname "${BASH_SOURCE[0]}"` && pwd)
source $ABSOLUTE_PATH/functions.sh

DISTRO=$1
PARENT="${DISTRO%-*}"                                                          
RELEASE="${DISTRO##*-}"

case $DISTRO in
    centos-*)
        note "Attempting to use yum to install."
        REPO_BASE=$LSTORE_RELEASE_BASE/repo/$PARENT/$RELEASE/
        cat > /etc/yum.repos.d/lstore.repo <<-EOF
# Autogenerated from lstore-release/scripts/rest-repo-internal.sh
[lstore]
name=LStore-\$releasever - LStore packages for \$basearch
baseurl=file://${REPO_BASE}
enabled=1
gpgcheck=0
protect=1
EOF
        yum install -y epel-release
        yum install -y accre-lio accre-gridftp globus-gridftp-server-progs which
        yum clean all
        ;;
    ubuntu-*|debian-*)
        note "Attempting lstore install from local apt repository."
        REPO_BASE=$LSTORE_RELEASE_BASE/repo/$PARENT/$RELEASE/
        # note that the trailing slash after 'packages' is crucial otherwise
        # that field is treated as a distro release name
cat > /etc/apt/sources.list.d/lstore.list <<-EOF
# Autogenerated from lstore-release/scripts/test-repo-internal.sh
deb file://${REPO_BASE}/  packages/
EOF
        apt-get update
        apt-get install -y --force-yes --no-install-recommends --no-upgrade \
                    accre-lio accre-gridftp globus-gridftp-server-progs \
                    accre-toolbox accre-gop accre-ibp libapr-accre1 \
                    libapr-accre-util1 accre-jerasure czmq libexpat1 vandy-cfg
        apt-get clean
        # --force-yes needed to install unsigned / self-signed packages The
        # second line of packages should be pulled in as deps and unnecessary
        # except that CPack support for automatically configuring .deb
        # dependencies is poor
        ;;
    *)
        fatal "Unknown distro ${DISTRO}."
        ;;
esac

# TODO: Need a better smoke test. If you don't know you have a server up, there
#       there isn't much that can be done. At the very least --version needs to
#       be added to the command line args for the tools.
note "Attempting ldd against lio_ls."
ldd $(which lio_ls)
note "Attempting to execute lio_ls."
lio_ls && [ $? -eq 1 ]
note "Loading gridftp plugin."
globus-gridftp-server -dsi lfs -debug -version -log-level all
