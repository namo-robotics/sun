#!/bin/bash
set -euo pipefail

# Build Debian package for Sun compiler
# Usage: ./scripts/build-deb.sh [--install]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_ROOT"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log() { echo -e "${GREEN}[build-deb]${NC} $1"; }
warn() { echo -e "${YELLOW}[build-deb]${NC} $1"; }
error() { echo -e "${RED}[build-deb]${NC} $1" >&2; }

# Check for required tools
check_dependencies() {
    local missing=()
    
    for cmd in dpkg-buildpackage debhelper cmake; do
        if ! command -v "$cmd" &> /dev/null && ! dpkg -l "$cmd" &> /dev/null 2>&1; then
            missing+=("$cmd")
        fi
    done
    
    if [ ${#missing[@]} -ne 0 ]; then
        error "Missing dependencies: ${missing[*]}"
        log "Install with: sudo apt-get install -y devscripts debhelper build-essential cmake"
        exit 1
    fi
}

# Compute dev version
compute_dev_version() {
    echo "0~dev"
}

# Update version in changelog from git tag or argument
# Defaults to 0~dev for non-tagged builds
update_version() {
    local version="${1:-}"
    local is_release=false
    
    if [ -z "$version" ]; then
        # Check if on a release tag (v0, v1, v2, ...)
        if git describe --tags --exact-match HEAD 2>/dev/null | grep -qE '^v[0-9]+$'; then
            version=$(git describe --tags --exact-match HEAD | sed 's/^v//')
            is_release=true
        else
            # Default to dev version
            version=$(compute_dev_version)
        fi
    fi
    
    # Add revision number for releases only
    local full_version="$version"
    if [ "$is_release" = true ]; then
        full_version="${version}-1"
    fi
    
    log "Building version: $full_version"
    
    # Update changelog with new version
    local date=$(date -R)
    local maintainer=$(grep "^Maintainer:" debian/control | sed 's/Maintainer: //')
    
    cat > debian/changelog << EOF
sun (${full_version}) unstable; urgency=medium

  * Release ${version}

 -- ${maintainer}  ${date}
EOF
}

# Build the package
build_package() {
    log "Installing build dependencies..."
    if [ "$(id -u)" -eq 0 ]; then
        apt-get update
        apt-get install -y devscripts debhelper build-essential cmake llvm-20-dev libzstd-dev
    else
        sudo apt-get update
        sudo apt-get install -y devscripts debhelper build-essential cmake llvm-20-dev libzstd-dev
    fi
    
    log "Building Debian package..."
    
    # Clean previous builds
    rm -rf obj-* debian/.debhelper debian/sun debian/files debian/*.debhelper* debian/*.substvars
    
    # Create a build directory to avoid parent directory permission issues
    local build_dir=$(mktemp -d)
    trap "rm -rf '$build_dir'" EXIT
    
    # Copy source to build directory
    cp -a . "$build_dir/sun"
    cd "$build_dir/sun"
    
    # Build the package (unsigned for CI)
    dpkg-buildpackage -us -uc -b
    
    # Move the built package to dist directory in original location
    mkdir -p "$PROJECT_ROOT/dist"
    mv "$build_dir"/*.deb "$PROJECT_ROOT/dist/" 2>/dev/null || true
    mv "$build_dir"/*.buildinfo "$PROJECT_ROOT/dist/" 2>/dev/null || true
    mv "$build_dir"/*.changes "$PROJECT_ROOT/dist/" 2>/dev/null || true
    
    cd "$PROJECT_ROOT"
    rm -rf "$build_dir"
    trap - EXIT
    
    log "Package built successfully!"
    ls -la dist/
}

# Install the package locally
install_package() {
    local deb_file=$(ls dist/*.deb 2>/dev/null | head -1)
    
    if [ -z "$deb_file" ]; then
        error "No .deb file found in dist/"
        exit 1
    fi
    
    log "Installing $deb_file..."
    if [ "$(id -u)" -eq 0 ]; then
        dpkg -i "$deb_file" || apt-get install -f -y
    else
        sudo dpkg -i "$deb_file" || sudo apt-get install -f -y
    fi
    
    log "Installation complete! Try: sun --help"
}

# Main
main() {
    local do_install=false
    local version=""
    
    while [[ $# -gt 0 ]]; do
        case $1 in
            --install|-i)
                do_install=true
                shift
                ;;
            --version|-v)
                version="$2"
                shift 2
                ;;
            --help|-h)
                echo "Usage: $0 [--install] [--version VERSION]"
                echo ""
                echo "Options:"
                echo "  --install, -i     Install the package after building"
                echo "  --version, -v     Set package version (default: 0~dev)"
                echo "  --help, -h        Show this help"
                exit 0
                ;;
            *)
                error "Unknown option: $1"
                exit 1
                ;;
        esac
    done
    
    check_dependencies
    update_version "$version"
    build_package
    
    if [ "$do_install" = true ]; then
        install_package
    fi
}

main "$@"
