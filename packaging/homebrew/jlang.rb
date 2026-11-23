# Homebrew formula for JLang. The release workflow copies it into the
# jafarnz/homebrew-tap repository with the version and checksum filled in:
#
#   brew install jafarnz/tap/jlang
class Jlang < Formula
  desc "Small language for learning how hardware works, with a board simulator and RISC-V backend"
  homepage "https://jafarniaz.com/jlang"
  url "https://github.com/jafarnz/JLang/archive/refs/tags/v@VERSION@.tar.gz"
  sha256 "@SHA256@"
  license "MIT"
  head "https://github.com/jafarnz/JLang.git", branch: "main"

  def install
    system "make"
    bin.install "build/jlang"
    pkgshare.install "examples"
  end

  test do
    (testpath/"hello.jlang").write("print(2 + 3)\n")
    assert_equal "5\n", shell_output("#{bin}/jlang run #{testpath}/hello.jlang")
  end
end
