class RobotweaxSrt < Formula
  desc "Independent Secure Reliable Transport implementation"
  homepage "https://github.com/Robotweax/srt"
  url "https://github.com/Robotweax/srt/archive/492a7d61390cbec86e44e177ec034f0f5d9a5cc3.tar.gz"
  version "0.2.5"
  sha256 "dc55b9e1c2583eec6ff5ab82db70de6bfb0cc9c6e18cfd1ed15376f61d8fca5f"
  license "MIT"

  depends_on "cmake" => :build
  depends_on "pkgconf" => :test
  depends_on "openssl@3"

  def install
    system "cmake", "-S", ".", "-B", "build", *std_cmake_args,
           "-DBUILD_SHARED_LIBS=ON",
           "-DROBOTWEAX_SRT_INSTALL_LAYOUT=namespaced",
           "-DROBOTWEAX_SRT_INSTALL_LIBSRT_PKGCONFIG_COMPAT=OFF",
           "-DROBOTWEAX_SRT_BUILD_TESTS=OFF",
           "-DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF",
           "-DROBOTWEAX_SRT_BUILD_TOOLS=OFF",
           "-DROBOTWEAX_SRT_BUILD_EXAMPLES=OFF",
           "-DROBOTWEAX_SRT_WARNINGS_AS_ERRORS=OFF",
           "-DROBOTWEAX_SRT_CRYPTO_BACKEND=openssl",
           "-DENABLE_AEAD_API_PREVIEW=OFF"
    system "cmake", "--build", "build"
    system "cmake", "--install", "build"
  end

  test do
    (testpath/"consumer.c").write <<~C
      #include <robotweax_srt.h>
      int main(void) {
        robotweax_srt_options *options = 0;
        if (robotweax_srt_options_create(&options) != ROBOTWEAX_SRT_OK || !options)
          return 1;
        robotweax_srt_options_destroy(options);
        return 0;
      }
    C
    flags = shell_output("pkg-config --cflags --libs robotweax-srt").split
    system ENV.cc, "consumer.c", "-o", "consumer", *flags
    system "./consumer"
    refute_path_exists include/"srt/srt.h"
    refute_path_exists lib/"pkgconfig/srt.pc"
    assert_path_exists include/"robotweax-srt/srt/srt.h"
    assert_equal version.to_s, shell_output("pkg-config --modversion robotweax-srt").strip
  end
end
