#!/usr/bin/env bash

# Build script for Play! libretro core for iOS/tvOS
# Supports Vulkan (via MoltenVK) and OpenGL ES

set -e
set -o pipefail

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$SCRIPT_DIR"
BUILD_DIR="$PROJECT_ROOT/build_libretro_ios"

# Default settings
PLATFORM="${PLATFORM:-iOS}"  # iOS or tvOS
CONFIGURATION="${CONFIGURATION:-Release}"
ENABLE_VULKAN="${ENABLE_VULKAN:-YES}"
ENABLE_GLES="${ENABLE_GLES:-YES}"
IOS_INTERPRETER_MODE="${IOS_INTERPRETER_MODE:-OFF}"
VULKAN_SDK="${VULKAN_SDK:-/Users/jmattiello/VulkanSDK/macOS}"

# Compiler flags - easily customizable
EXTRA_CFLAGS="${EXTRA_CFLAGS:-}"
EXTRA_CXXFLAGS="${EXTRA_CXXFLAGS:-}"
EXTRA_LDFLAGS="${EXTRA_LDFLAGS:-}"

# iOS/tvOS 15+ optimizations
CFLAGS_OPTIMIZATIONS=""
CXXFLAGS_OPTIMIZATIONS=""

# FMV-optimized flags for Release builds
setup_optimization_flags() {
    local arch="arm64"
    local ios_min_version="$IOS_DEPLOYMENT_TARGET"

    if [[ "$CONFIGURATION" == "Release" ]]; then
        log_info "Setting up FMV-optimized flags for Release build..."

        # Core optimization flags
        local base_flags="-arch ${arch} \
-DIOS \
-DTARGET_NO_NIXPROF \
-DTARGET_OS_IOS=1 \
-miphoneos-version-min=${ios_min_version} \
-fdata-sections \
-ffast-math \
-ffunction-sections \
-finline-functions \
-flto=thin \
-fno-strict-aliasing \
-fomit-frame-pointer \
-fpermissive \
-ftree-vectorize \
-funsafe-math-optimizations \
-fvectorize \
-march=armv8-a+simd+crc+crypto \
-mcpu=apple-a10 \
-mtune=apple-a14 \
-Ofast \
-fno-math-errno \
-ffinite-math-only \
-fno-signed-zeros \
-fno-trapping-math \
-freciprocal-math \
-ffp-contract=fast \
-funroll-loops \
-DARM_NEON \
-DHAVE_NEON \
-DTARGET_IPHONE"

        # Clang-compatible aggressive optimization flags (conservative set)
        local aggressive_flags="-fno-stack-protector \
-funroll-loops \
-fvectorize \
-fslp-vectorize \
-fomit-frame-pointer \
-finline-functions \
-fstrict-aliasing \
-fmerge-all-constants \
-fno-common \
-fdata-sections \
-ffunction-sections \
-falign-functions=32 \
-falign-loops=32"

        # Set C flags
        CFLAGS_OPTIMIZATIONS="$base_flags"

        # Set C++ flags (add C++17 standard)
        CXXFLAGS_OPTIMIZATIONS="$base_flags -std=c++17"

        # Add aggressive flags if enabled
        if [[ "${AGGRESSIVE_FLAGS:-OFF}" == "ON" ]]; then
            log_info "Adding aggressive optimization flags..."
            CFLAGS_OPTIMIZATIONS="$CFLAGS_OPTIMIZATIONS $aggressive_flags"
            CXXFLAGS_OPTIMIZATIONS="$CXXFLAGS_OPTIMIZATIONS $aggressive_flags"
        fi

        log_success "FMV-optimized flags configured for Release build"
    else
        log_info "Using default flags for $CONFIGURATION build"
    fi
}

# iOS/tvOS deployment targets
IOS_DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET:-16.4}"
TVOS_DEPLOYMENT_TARGET="${TVOS_DEPLOYMENT_TARGET:-16.4}"

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

print_usage() {
    cat << EOF
Usage: $0 [OPTIONS]

Build Play! libretro core for iOS/tvOS

OPTIONS:
    -p, --platform PLATFORM     Target platform: iOS or tvOS (default: iOS)
    -c, --config CONFIG          Build configuration: Release or Debug (default: Release)
    -v, --vulkan ENABLE          Enable Vulkan support: YES or NO (default: YES)
    -g, --gles ENABLE            Enable OpenGL ES support: YES or NO (default: YES)
    --ios-interpreter            Enable MIPT-MIPS interpreter backend for iOS compatibility
    --disable-ios-interpreter    Disable MIPT-MIPS interpreter backend (default)
    -s, --vulkan-sdk PATH        Path to Vulkan SDK (default: /Users/jmattiello/VulkanSDK/macOS)
    --ios-target VERSION         iOS deployment target (default: 16.4)
    --tvos-target VERSION        tvOS deployment target (default: 16.4)
    --extra-cflags FLAGS         Additional C compiler flags
    --extra-cxxflags FLAGS       Additional C++ compiler flags
    --extra-ldflags FLAGS        Additional linker flags
    --aggressive-flags           Enable aggressive optimization flags for Release builds
    --clean                      Clean build directory before building
    -h, --help                   Show this help message

ENVIRONMENT VARIABLES:
    PLATFORM                     Same as --platform
    CONFIGURATION                Same as --config
    ENABLE_VULKAN                Same as --vulkan
    ENABLE_GLES                  Same as --gles
    IOS_INTERPRETER_MODE         Same as --ios-interpreter
    VULKAN_SDK                   Same as --vulkan-sdk
    EXTRA_CFLAGS                 Same as --extra-cflags
    EXTRA_CXXFLAGS               Same as --extra-cxxflags
    EXTRA_LDFLAGS                Same as --extra-ldflags

EXAMPLES:
    # Build for iOS with Vulkan and GLES
    $0

    # Build for tvOS with custom flags
    $0 --platform tvOS --extra-cxxflags "-O3 -DNDEBUG"

    # Clean build for iOS without Vulkan
    $0 --clean --vulkan NO

    # Release build with FMV-optimized flags
    $0 --config Release

    # Maximum performance Release build with aggressive optimizations
    $0 --config Release --aggressive-flags

EOF
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -p|--platform)
            PLATFORM="$2"
            shift 2
            ;;
        -c|--config)
            CONFIGURATION="$2"
            shift 2
            ;;
        -v|--vulkan)
            ENABLE_VULKAN="$2"
            shift 2
            ;;
        -g|--gles)
            ENABLE_GLES="$2"
            shift 2
            ;;
        --ios-interpreter)
            IOS_INTERPRETER_MODE="ON"
            shift
            ;;
        --disable-ios-interpreter)
            IOS_INTERPRETER_MODE="OFF"
            shift
            ;;
        -s|--vulkan-sdk)
            VULKAN_SDK="$2"
            shift 2
            ;;
        --ios-target)
            IOS_DEPLOYMENT_TARGET="$2"
            shift 2
            ;;
        --tvos-target)
            TVOS_DEPLOYMENT_TARGET="$2"
            shift 2
            ;;
        --extra-cflags)
            EXTRA_CFLAGS="$2"
            shift 2
            ;;
        --extra-cxxflags)
            EXTRA_CXXFLAGS="$2"
            shift 2
            ;;
        --extra-ldflags)
            EXTRA_LDFLAGS="$2"
            shift 2
            ;;
        --clean)
            CLEAN_BUILD=YES
            shift
            ;;
        --aggressive-flags)
            AGGRESSIVE_FLAGS=ON
            shift
            ;;
        -h|--help)
            print_usage
            exit 0
            ;;
        *)
            log_error "Unknown option: $1"
            print_usage
            exit 1
            ;;
    esac
done

# Validate platform
if [[ "$PLATFORM" != "iOS" && "$PLATFORM" != "tvOS" ]]; then
    log_error "Invalid platform: $PLATFORM. Must be 'iOS' or 'tvOS'"
    exit 1
fi

# Validate configuration
if [[ "$CONFIGURATION" != "Release" && "$CONFIGURATION" != "Debug" ]]; then
    log_error "Invalid configuration: $CONFIGURATION. Must be 'Release' or 'Debug'"
    exit 1
fi

# Check dependencies
check_dependencies() {
    log_info "Checking dependencies..."

    if ! command -v cmake &> /dev/null; then
        log_error "CMake not found. Install with: brew install cmake"
        exit 1
    fi

    if ! command -v xcodebuild &> /dev/null; then
        log_error "Xcode not found. Please install Xcode."
        exit 1
    fi

    # Check Vulkan SDK if enabled
    if [[ "$ENABLE_VULKAN" == "YES" ]]; then
        if [[ ! -d "$VULKAN_SDK" ]]; then
            log_warning "Vulkan SDK not found at: $VULKAN_SDK"
            log_warning "Disabling Vulkan support. Set VULKAN_SDK environment variable or use --vulkan-sdk option."
            ENABLE_VULKAN=NO
        else
            log_info "Using Vulkan SDK: $VULKAN_SDK"
        fi
    fi

    log_success "Dependencies check completed"
}

# Setup build directory
setup_build_dir() {
    if [[ "$CLEAN_BUILD" == "YES" && -d "$BUILD_DIR" ]]; then
        log_info "Cleaning build directory..."
        rm -rf "$BUILD_DIR"
    fi

    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
}

# Configure CMake
configure_cmake() {
    log_info "Configuring CMake for $PLATFORM ($CONFIGURATION)..."

    # Base CMake arguments
    CMAKE_ARGS=(
        ".."
        "-G" "Xcode"
        "-DBUILD_LIBRETRO_CORE=ON"
        "-DBUILD_PLAY=OFF"
        "-DCMAKE_BUILD_TYPE=$CONFIGURATION"
        "-DTARGET_IOS=ON"
    )

    # Platform-specific settings
    if [[ "$PLATFORM" == "iOS" ]]; then
        CMAKE_ARGS+=(
            "-DIOS_PLATFORM=OS"
            "-DCMAKE_OSX_DEPLOYMENT_TARGET=$IOS_DEPLOYMENT_TARGET"
        )
    else # tvOS
        CMAKE_ARGS+=(
            "-DIOS_PLATFORM=TVOS"
            "-DCMAKE_OSX_DEPLOYMENT_TARGET=$TVOS_DEPLOYMENT_TARGET"
        )
    fi

    # Vulkan support
    if [[ "$ENABLE_VULKAN" == "YES" ]]; then
        CMAKE_ARGS+=(
            "-DCMAKE_PREFIX_PATH=$VULKAN_SDK"
            "-DVULKAN_SDK=$VULKAN_SDK"
        )
        log_info "Vulkan support enabled"
    else
        log_info "Vulkan support disabled"
    fi

    # iOS interpreter mode support
    if [[ "$IOS_INTERPRETER_MODE" == "ON" ]]; then
        CMAKE_ARGS+=(
            "-DIOS_INTERPRETER_MODE=ON"
        )
        log_info "iOS interpreter mode enabled - all JIT disabled"
    else
        log_info "iOS interpreter mode disabled - JIT enabled"
    fi

    # Add extra compiler flags
    CMAKE_ARGS+=("-DCMAKE_C_FLAGS=$CFLAGS_OPTIMIZATIONS")
    CMAKE_ARGS+=("-DCMAKE_CXX_FLAGS=$CXXFLAGS_OPTIMIZATIONS")

    if [[ -n "$EXTRA_CFLAGS" ]]; then
        CMAKE_ARGS+=("-DCMAKE_C_FLAGS=$EXTRA_CFLAGS")
    fi

    if [[ -n "$EXTRA_CXXFLAGS" ]]; then
        CMAKE_ARGS+=("-DCMAKE_CXX_FLAGS=$EXTRA_CXXFLAGS")
    fi

    if [[ -n "$EXTRA_LDFLAGS" ]]; then
        CMAKE_ARGS+=("-DCMAKE_EXE_LINKER_FLAGS=$EXTRA_LDFLAGS")
        CMAKE_ARGS+=("-DCMAKE_SHARED_LINKER_FLAGS=$EXTRA_LDFLAGS")
    fi

    # Look for iOS toolchain file in common locations
    TOOLCHAIN_LOCATIONS=(
        "../deps/Dependencies/cmake-ios/ios.cmake"
        "../cmake/ios.cmake"
        "../toolchain/ios.cmake"
    )

    TOOLCHAIN_FILE=""
    for location in "${TOOLCHAIN_LOCATIONS[@]}"; do
        if [[ -f "$location" ]]; then
            TOOLCHAIN_FILE="$location"
            break
        fi
    done

    if [[ -n "$TOOLCHAIN_FILE" ]]; then
        CMAKE_ARGS+=("-DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN_FILE")
        log_info "Using iOS toolchain: $TOOLCHAIN_FILE"
    else
        log_warning "iOS toolchain file not found. Using Xcode's built-in iOS support."
        # Add iOS-specific flags manually
        CMAKE_ARGS+=(
            "-DCMAKE_SYSTEM_NAME=iOS"
            "-DCMAKE_OSX_ARCHITECTURES=arm64"
        )
    fi

    # Disable code signing for iOS builds
    CMAKE_ARGS+=(
        "-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED=NO"
        "-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO"
        "-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY=\"\""
        "-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_STYLE=Manual"
        "-DCMAKE_XCODE_ATTRIBUTE_SKIP_INSTALL=YES"
    )

    log_info "CMake configuration:"
    printf '%s\n' "${CMAKE_ARGS[@]}" | sed 's/^/  /'

    cmake "${CMAKE_ARGS[@]}"
}

# Build the project
build_project() {
    log_info "Building Play! libretro core..."

    cmake --build . --config "$CONFIGURATION" --target play_libretro

    if [[ $? -eq 0 ]]; then
        log_success "Build completed successfully!"

        # Find the built dylib in the correct configuration directory
        if [[ "$PLATFORM" == "tvOS" ]]; then
            DYLIB_PATTERN="*play_libretro*tvos.dylib"
        else
            DYLIB_PATTERN="*play_libretro*ios.dylib"
        fi

        # Look specifically in the configuration directory (Debug-iphoneos or Release-iphoneos)
        # Exclude dSYM files to find the actual dylib
        CONFIG_DIR="$CONFIGURATION-iphoneos"
        BUILT_DYLIB=$(find . -path "*/$CONFIG_DIR/*" -name "$DYLIB_PATTERN" -not -path "*.dSYM/*" -type f | head -1)

        if [[ -n "$BUILT_DYLIB" ]]; then
            log_success "Built dylib: $BUILT_DYLIB"

            # Copy to project root for convenience
            OUTPUT_NAME="play_libretro_${PLATFORM,,}.dylib"
            cp "$BUILT_DYLIB" "../$OUTPUT_NAME"

            # Strip any signatures and clean extended attributes to prevent framework issues
            log_info "Cleaning dylib signatures and attributes..."
            codesign --remove-signature "../$OUTPUT_NAME" 2>/dev/null || true
            xattr -c "../$OUTPUT_NAME" 2>/dev/null || true

            # Verify the dylib is unsigned
            if codesign -dv "../$OUTPUT_NAME" 2>&1 | grep -q "not signed"; then
                log_info "Dylib is properly unsigned"
            else
                log_warning "Dylib may still have signature remnants"
            fi

            log_success "Copied to: $PROJECT_ROOT/$OUTPUT_NAME"

            # Show file info
            log_info "File information:"
            file "../$OUTPUT_NAME" | sed 's/^/  /'
            ls -lh "../$OUTPUT_NAME" | sed 's/^/  /'
        else
            log_warning "Built dylib not found with pattern: $DYLIB_PATTERN"
        fi
    else
        log_error "Build failed!"
        exit 1
    fi
}

# Print build summary
print_summary() {
    log_info "Build Summary:"
    echo "  Platform: $PLATFORM"
    echo "  Configuration: $CONFIGURATION"
    echo "  Vulkan Support: $ENABLE_VULKAN"
    echo "  OpenGL ES Support: $ENABLE_GLES"
    echo "  iOS Interpreter Mode: $IOS_INTERPRETER_MODE"
    if [[ "$ENABLE_VULKAN" == "YES" ]]; then
        echo "  Vulkan SDK: $VULKAN_SDK"
    fi
    if [[ -n "$EXTRA_CFLAGS" ]]; then
        echo "  Extra C Flags: $EXTRA_CFLAGS"
    fi
    if [[ -n "$EXTRA_CXXFLAGS" ]]; then
        echo "  Extra C++ Flags: $EXTRA_CXXFLAGS"
    fi
    if [[ -n "$EXTRA_LDFLAGS" ]]; then
        echo "  Extra LD Flags: $EXTRA_LDFLAGS"
    fi
    echo ""
}

# Main execution
main() {
    log_info "Starting Play! libretro build for $PLATFORM..."
    print_summary

    check_dependencies
    setup_optimization_flags
    setup_build_dir
    configure_cmake
    build_project

    log_success "Build process completed!"
}

# Run main function
main "$@"
