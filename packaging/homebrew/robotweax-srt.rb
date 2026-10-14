class RobotweaxSrt < Formula
  desc "Robotweax Secure Reliable Transport"
  homepage "https://github.com/Robotweax/srt"
  url "https://github.com/Robotweax/srt/archive/1e7d7824a0420dac8e8ccaa950668ee301f99b9f.tar.gz"
  version "0.3.0"
  sha256 "046813c64242afec5fc1393790f04094cb4c8eecb3a769b49eed77632046901b"
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
    ENV.prepend_path "PKG_CONFIG_PATH", formula_opt_lib("openssl@3")/"pkgconfig"

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
