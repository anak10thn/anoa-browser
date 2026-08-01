class AnoaBrowserLinux < Formula
  desc "Headless browser built on Qt6/QWebEngine with CDP support"
  homepage "https://github.com/porcupine-md/anoa-browser"
  version "{{VERSION}}"
  sha256 "{{LINUX_SHA256}}"
  license "MIT"

  url "https://github.com/porcupine-md/anoa-browser/releases/download/v#{version}/anoa-browser-linux-x86_64.tar.gz"

  def install
    libexec.install Dir["*"]
    # anoa-browser.sh sets LD_LIBRARY_PATH / QtWebEngine paths relative to its
    # own (symlink-resolved) location, so a plain symlink is enough. The
    # terminal viewer is a subcommand of the same binary (`anoa-browser
    # terminal`), so this is the only launcher the bundle needs.
    bin.install_symlink libexec/"anoa-browser.sh" => "anoa-browser"
  end

  test do
    assert_match version.to_s, shell_output("#{bin}/anoa-browser --version")
  end
end
