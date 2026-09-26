Name:           robotweax-srt
Version:        0.2.6
Release:        1%{?dist}
Summary:        Robotweax Secure Reliable Transport library
License:        MIT
URL:            https://github.com/Robotweax/srt
Source0:        https://github.com/Robotweax/srt/archive/7ecb60ea8b4faca01ed86237b0cc9dc906350f6e.tar.gz
BuildRequires:  cmake
BuildRequires:  gcc-c++
BuildRequires:  openssl-devel >= 3.0
BuildRequires:  pkgconfig

%description
Robotweax SRT is an independent Secure Reliable Transport implementation.
Its library and metadata use names distinct from Haivision's SRT package.

%package devel
Summary:        Development files for Robotweax SRT
Requires:       %{name}%{?_isa} = %{version}-%{release}
Requires:       openssl-devel

%description devel
Headers, CMake target and pkg-config metadata for Robotweax SRT.
Consumers explicitly select this provider at build time.

%prep
%autosetup -n srt-7ecb60ea8b4faca01ed86237b0cc9dc906350f6e

%build
%cmake \
  -DBUILD_SHARED_LIBS=ON \
  -DROBOTWEAX_SRT_INSTALL_LAYOUT=namespaced \
  -DROBOTWEAX_SRT_INSTALL_LIBSRT_PKGCONFIG_COMPAT=OFF \
  -DROBOTWEAX_SRT_BUILD_TESTS=OFF \
  -DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF \
  -DROBOTWEAX_SRT_BUILD_TOOLS=OFF \
  -DROBOTWEAX_SRT_BUILD_EXAMPLES=OFF \
  -DROBOTWEAX_SRT_WARNINGS_AS_ERRORS=OFF \
  -DROBOTWEAX_SRT_CRYPTO_BACKEND=openssl \
  -DENABLE_AEAD_API_PREVIEW=OFF
%cmake_build

%install
%cmake_install

%files
%license LICENSE
%{_libdir}/librobotweax-srt.so.0.2*

%files devel
%{_includedir}/robotweax-srt/
%{_libdir}/librobotweax-srt.so
%{_libdir}/cmake/RobotweaxSRT/
%{_libdir}/pkgconfig/robotweax-srt.pc
%{_datadir}/doc/robotweax_srt/
%{_datadir}/robotweax_srt/abi/

%changelog
* Sat Sep 26 2026 Robotweax GmbH <srtpioneer@proton.me> - 0.2.6-1
- Package immutable 0.2.6 performance optimization source

* Thu Sep 24 2026 Robotweax GmbH <srtpioneer@proton.me> - 0.2.5-1
- Package immutable 0.2.5 source with namespaced install layout
